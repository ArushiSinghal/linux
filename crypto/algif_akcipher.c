/*
 * algif_akcipher: User-space interface for asymmetric cipher algorithms
 *
 * Copyright (C) 2015, Stephan Mueller <smueller@chronox.de>
 *
 * This file provides the user-space API for asymmetric ciphers.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <crypto/akcipher.h>
#include <crypto/scatterwalk.h>
#include <crypto/if_alg.h>
#include <crypto/public_key.h>
#include <keys/asymmetric-type.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <net/sock.h>

struct akcipher_sg_list {
	unsigned int cur;
	struct scatterlist sg[ALG_MAX_PAGES];
};

struct akcipher_tfm {
	struct crypto_akcipher *akcipher;
	char keyid[12];
	bool has_key;
};

struct akcipher_ctx {
	struct akcipher_sg_list tsgl;
	struct af_alg_sgl rsgl[ALG_MAX_PAGES];

	struct af_alg_completion completion;
	struct key *key;

	unsigned long used;

	unsigned int len;
	bool more;
	bool merge;
	int op;

	struct akcipher_request req;
};

static inline int akcipher_sndbuf(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;

	return max_t(int, max_t(int, sk->sk_sndbuf & PAGE_MASK, PAGE_SIZE) -
			  ctx->used, 0);
}

static inline bool akcipher_writable(struct sock *sk)
{
	return akcipher_sndbuf(sk) >= PAGE_SIZE;
}

static inline int akcipher_calcsize(struct akcipher_ctx *ctx)
{
	return crypto_akcipher_maxsize(crypto_akcipher_reqtfm(&ctx->req));
}

static void akcipher_put_sgl(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	struct akcipher_sg_list *sgl = &ctx->tsgl;
	struct scatterlist *sg = sgl->sg;
	unsigned int i;

	for (i = 0; i < sgl->cur; i++) {
		if (!sg_page(sg + i))
			continue;

		put_page(sg_page(sg + i));
		sg_assign_page(sg + i, NULL);
	}
	sg_init_table(sg, ALG_MAX_PAGES);
	sgl->cur = 0;
	ctx->used = 0;
	ctx->more = 0;
	ctx->merge = 0;
}

static void akcipher_wmem_wakeup(struct sock *sk)
{
	struct socket_wq *wq;

	if (!akcipher_writable(sk))
		return;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (wq_has_sleeper(&wq->wait))
		wake_up_interruptible_sync_poll(&wq->wait, POLLIN |
							   POLLRDNORM |
							   POLLRDBAND);
	sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_IN);
	rcu_read_unlock();
}

static int akcipher_wait_for_data(struct sock *sk, unsigned int flags)
{
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	long timeout;
	DEFINE_WAIT(wait);
	int err = -ERESTARTSYS;

	if (flags & MSG_DONTWAIT)
		return -EAGAIN;

	set_bit(SOCKWQ_ASYNC_WAITDATA, &sk->sk_socket->flags);

	for (;;) {
		if (signal_pending(current))
			break;
		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (sk_wait_event(sk, &timeout, !ctx->more)) {
			err = 0;
			break;
		}
	}
	finish_wait(sk_sleep(sk), &wait);

	clear_bit(SOCKWQ_ASYNC_WAITDATA, &sk->sk_socket->flags);

	return err;
}

static void akcipher_data_wakeup(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	struct socket_wq *wq;

	if (ctx->more)
		return;
	if (!ctx->used)
		return;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (wq_has_sleeper(&wq->wait))
		wake_up_interruptible_sync_poll(&wq->wait, POLLOUT |
							   POLLRDNORM |
							   POLLRDBAND);
	sk_wake_async(sk, SOCK_WAKE_SPACE, POLL_OUT);
	rcu_read_unlock();
}

static int akcipher_sendmsg(struct socket *sock, struct msghdr *msg,
			    size_t size)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	struct akcipher_sg_list *sgl = &ctx->tsgl;
	struct af_alg_control con = {};
	long copied = 0;
	int op = 0;
	bool init = 0;
	int err;

	if (msg->msg_controllen) {
		err = af_alg_cmsg_send(msg, &con);
		if (err)
			return err;

		init = 1;
		switch (con.op) {
		case ALG_OP_VERIFY:
		case ALG_OP_SIGN:
		case ALG_OP_ENCRYPT:
		case ALG_OP_DECRYPT:
			op = con.op;
			break;
		default:
			return -EINVAL;
		}
	}

	lock_sock(sk);
	if (!ctx->more && ctx->used)
		goto unlock;

	if (init)
		ctx->op = op;

	while (size) {
		unsigned long len = size;
		struct scatterlist *sg = NULL;

		/* use the existing memory in an allocated page */
		if (ctx->merge) {
			sg = sgl->sg + sgl->cur - 1;
			len = min_t(unsigned long, len,
				    PAGE_SIZE - sg->offset - sg->length);
			err = memcpy_from_msg(page_address(sg_page(sg)) +
					      sg->offset + sg->length,
					      msg, len);
			if (err)
				goto unlock;

			sg->length += len;
			ctx->merge = (sg->offset + sg->length) &
				     (PAGE_SIZE - 1);

			ctx->used += len;
			copied += len;
			size -= len;
			continue;
		}

		if (!akcipher_writable(sk)) {
			/* user space sent too much data */
			akcipher_put_sgl(sk);
			err = -EMSGSIZE;
			goto unlock;
		}

		/* allocate a new page */
		len = min_t(unsigned long, size, akcipher_sndbuf(sk));
		while (len) {
			int plen = 0;

			if (sgl->cur >= ALG_MAX_PAGES) {
				akcipher_put_sgl(sk);
				err = -E2BIG;
				goto unlock;
			}

			sg = sgl->sg + sgl->cur;
			plen = min_t(int, len, PAGE_SIZE);

			sg_assign_page(sg, alloc_page(GFP_KERNEL));
			if (!sg_page(sg)) {
				err = -ENOMEM;
				goto unlock;
			}

			err = memcpy_from_msg(page_address(sg_page(sg)),
					      msg, plen);
			if (err) {
				__free_page(sg_page(sg));
				sg_assign_page(sg, NULL);
				goto unlock;
			}

			sg->offset = 0;
			sg->length = plen;
			len -= plen;
			ctx->used += plen;
			copied += plen;
			sgl->cur++;
			size -= plen;
			ctx->merge = plen & (PAGE_SIZE - 1);
		}
	}

	err = 0;

	ctx->more = msg->msg_flags & MSG_MORE;

unlock:
	akcipher_data_wakeup(sk);
	release_sock(sk);

	return err ?: copied;
}

static ssize_t akcipher_sendpage(struct socket *sock, struct page *page,
				 int offset, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	struct akcipher_sg_list *sgl = &ctx->tsgl;
	int err = 0;

	if (flags & MSG_SENDPAGE_NOTLAST)
		flags |= MSG_MORE;

	if (sgl->cur >= ALG_MAX_PAGES)
		return -E2BIG;

	lock_sock(sk);
	if (!ctx->more && ctx->used)
		goto unlock;

	if (!size)
		goto done;

	if (!akcipher_writable(sk)) {
		/* user space sent too much data */
		akcipher_put_sgl(sk);
		err = -EMSGSIZE;
		goto unlock;
	}

	ctx->merge = 0;

	get_page(page);
	sg_set_page(sgl->sg + sgl->cur, page, size, offset);
	sgl->cur++;
	ctx->used += size;

done:
	ctx->more = flags & MSG_MORE;
unlock:
	akcipher_data_wakeup(sk);
	release_sock(sk);

	return err ? err : size;
}

static int asym_key_encrypt(const struct key *key, struct akcipher_request *req)
{
	struct kernel_pkey_params params = {0};
	char *src = NULL, *dst = NULL, *in, *out;
	int ret;

	if (!sg_is_last(req->src)) {
		src = kmalloc(req->src_len, GFP_KERNEL);
		if (!src)
			return -ENOMEM;
		scatterwalk_map_and_copy(src, req->src, 0, req->src_len, 0);
		in = src;
	} else {
		in = sg_virt(req->src);
	}
	if (!sg_is_last(req->dst)) {
		dst = kmalloc(req->dst_len, GFP_KERNEL);
		if (!dst) {
			kfree(src);
			return -ENOMEM;
		}
		out = dst;
	} else {
		out = sg_virt(req->dst);
	}
	params.key = (struct key *)key;
	params.data_len = req->src_len;
	params.enc_len = req->dst_len;
	ret = encrypt_blob(&params, in, out);
	if (ret)
		goto free;

	if (dst)
		scatterwalk_map_and_copy(dst, req->dst, 0, req->dst_len, 1);
free:
	kfree(src);
	kfree(dst);
	return ret;
}

static int asym_key_decrypt(const struct key *key, struct akcipher_request *req)
{
	struct kernel_pkey_params params = {0};
	char *src = NULL, *dst = NULL, *in, *out;
	int ret;

	if (!sg_is_last(req->src)) {
		src = kmalloc(req->src_len, GFP_KERNEL);
		if (!src)
			return -ENOMEM;
		scatterwalk_map_and_copy(src, req->src, 0, req->src_len, 0);
		in = src;
	} else {
		in = sg_virt(req->src);
	}
	if (!sg_is_last(req->dst)) {
		dst = kmalloc(req->dst_len, GFP_KERNEL);
		if (!dst) {
			kfree(src);
			return -ENOMEM;
		}
		out = dst;
	} else {
		out = sg_virt(req->dst);
	}
	params.key = (struct key *)key;
	params.data_len = req->src_len;
	params.enc_len = req->dst_len;
	ret = decrypt_blob(&params, in, out);
	if (ret)
		goto free;

	if (dst)
		scatterwalk_map_and_copy(dst, req->dst, 0, req->dst_len, 1);
free:
	kfree(src);
	kfree(dst);
	return ret;
}

static int asym_key_sign(const struct key *key, struct akcipher_request *req)
{
	struct kernel_pkey_params params = {0};
	char *src = NULL, *dst = NULL, *in, *out;
	int ret;

	if (!sg_is_last(req->src)) {
		src = kmalloc(req->src_len, GFP_KERNEL);
		if (!src)
			return -ENOMEM;
		scatterwalk_map_and_copy(src, req->src, 0, req->src_len, 0);
		in = src;
	} else {
		in = sg_virt(req->src);
	}
	if (!sg_is_last(req->dst)) {
		dst = kmalloc(req->dst_len, GFP_KERNEL);
		if (!dst) {
			kfree(src);
			return -ENOMEM;
		}
		out = dst;
	} else {
		out = sg_virt(req->dst);
	}
	params.key = (struct key *)key;
	params.data_len = req->src_len;
	params.enc_len = req->dst_len;
	ret = create_signature(&params, in, out);
	if (ret)
		goto free;

	if (dst)
		scatterwalk_map_and_copy(dst, req->dst, 0, req->dst_len, 1);
free:
	kfree(src);
	kfree(dst);
	return ret;
}

static int asym_key_verify(const struct key *key, struct akcipher_request *req)
{
	struct public_key_signature sig;
	char *src = NULL, *in;
	int ret;

	if (!sg_is_last(req->src)) {
		src = kmalloc(req->src_len, GFP_KERNEL);
		if (!src)
			return -ENOMEM;
		scatterwalk_map_and_copy(src, req->src, 0, req->src_len, 0);
		in = src;
	} else {
		in = sg_virt(req->src);
	}
	sig.pkey_algo = "rsa";
	sig.encoding = "pkcs1";
	/* Need to find a way to pass the hash param */
	sig.hash_algo = "sha1";
	sig.digest_size = 20;
	sig.s_size = req->src_len;
	sig.s = src;
	ret = verify_signature(key, &sig);
	kfree(src);
	return ret;
}

static int akcipher_recvmsg(struct socket *sock, struct msghdr *msg,
			    size_t ignored, int flags)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	struct akcipher_sg_list *sgl = &ctx->tsgl;
	unsigned int i = 0;
	int err;
	unsigned long used = 0;
	size_t usedpages = 0;
	unsigned int cnt = 0;

	/* Limit number of IOV blocks to be accessed below */
	if (msg->msg_iter.nr_segs > ALG_MAX_PAGES)
		return -ENOMSG;

	lock_sock(sk);

	if (ctx->more) {
		err = akcipher_wait_for_data(sk, flags);
		if (err)
			goto unlock;
	}

	used = ctx->used;

	/* convert iovecs of output buffers into scatterlists */
	while (iov_iter_count(&msg->msg_iter)) {
		/* make one iovec available as scatterlist */
		err = af_alg_make_sg(&ctx->rsgl[cnt], &msg->msg_iter,
				     iov_iter_count(&msg->msg_iter));
		if (err < 0)
			goto unlock;
		usedpages += err;
		/* chain the new scatterlist with previous one */
		if (cnt)
			af_alg_link_sg(&ctx->rsgl[cnt - 1], &ctx->rsgl[cnt]);

		iov_iter_advance(&msg->msg_iter, err);
		cnt++;
	}

	/* ensure output buffer is sufficiently large */
	if (usedpages < akcipher_calcsize(ctx)) {
		err = -EMSGSIZE;
		goto unlock;
	}

	sg_mark_end(sgl->sg + sgl->cur - 1);

	akcipher_request_set_crypt(&ctx->req, sgl->sg, ctx->rsgl[0].sg, used,
				   usedpages);
	switch (ctx->op) {
	case ALG_OP_VERIFY:
		if (ctx->key)
			err = asym_key_verify(ctx->key, &ctx->req);
		else
			err = crypto_akcipher_verify(&ctx->req);
		break;
	case ALG_OP_SIGN:
		if (ctx->key)
			err = asym_key_sign(ctx->key, &ctx->req);
		else
			err = crypto_akcipher_sign(&ctx->req);
		break;
	case ALG_OP_ENCRYPT:
		if (ctx->key)
			err = asym_key_encrypt(ctx->key, &ctx->req);
		else
			err = crypto_akcipher_encrypt(&ctx->req);
		break;
	case ALG_OP_DECRYPT:
		if (ctx->key)
			err = asym_key_decrypt(ctx->key, &ctx->req);
		else
			err = crypto_akcipher_decrypt(&ctx->req);
		break;
	default:
		err = -EFAULT;
		goto unlock;
	}

	err = af_alg_wait_for_completion(err, &ctx->completion);

	if (err) {
		/* EBADMSG implies a valid cipher operation took place */
		if (err == -EBADMSG)
			akcipher_put_sgl(sk);
		goto unlock;
	}

	akcipher_put_sgl(sk);

unlock:
	for (i = 0; i < cnt; i++)
		af_alg_free_sg(&ctx->rsgl[i]);

	akcipher_wmem_wakeup(sk);
	release_sock(sk);

	return err ? err : ctx->req.dst_len;
}

static unsigned int akcipher_poll(struct file *file, struct socket *sock,
				  poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;
	unsigned int mask = 0;

	sock_poll_wait(file, sk_sleep(sk), wait);

	if (!ctx->more)
		mask |= POLLIN | POLLRDNORM;

	if (akcipher_writable(sk))
		mask |= POLLOUT | POLLWRNORM | POLLWRBAND;

	return mask;
}

static struct proto_ops algif_akcipher_ops = {
	.family		=	PF_ALG,

	.connect	=	sock_no_connect,
	.socketpair	=	sock_no_socketpair,
	.getname	=	sock_no_getname,
	.ioctl		=	sock_no_ioctl,
	.listen		=	sock_no_listen,
	.shutdown	=	sock_no_shutdown,
	.getsockopt	=	sock_no_getsockopt,
	.mmap		=	sock_no_mmap,
	.bind		=	sock_no_bind,
	.accept		=	sock_no_accept,
	.setsockopt	=	sock_no_setsockopt,

	.release	=	af_alg_release,
	.sendmsg	=	akcipher_sendmsg,
	.sendpage	=	akcipher_sendpage,
	.recvmsg	=	akcipher_recvmsg,
	.poll		=	akcipher_poll,
};

static int akcipher_check_key(struct socket *sock)
{
	int err = 0;
	struct sock *psk;
	struct alg_sock *pask;
	struct akcipher_tfm *tfm;
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);

	lock_sock(sk);
	if (ask->refcnt)
		goto unlock_child;

	psk = ask->parent;
	pask = alg_sk(ask->parent);
	tfm = pask->private;

	err = -ENOKEY;
	lock_sock_nested(psk, SINGLE_DEPTH_NESTING);
	if (!tfm->has_key)
		goto unlock;

	if (!pask->refcnt++)
		sock_hold(psk);

	ask->refcnt = 1;
	sock_put(psk);

	err = 0;

unlock:
	release_sock(psk);
unlock_child:
	release_sock(sk);

	return err;
}

static int akcipher_sendmsg_nokey(struct socket *sock, struct msghdr *msg,
				  size_t size)
{
	int err;

	err = akcipher_check_key(sock);
	if (err)
		return err;

	return akcipher_sendmsg(sock, msg, size);
}

static ssize_t akcipher_sendpage_nokey(struct socket *sock, struct page *page,
				       int offset, size_t size, int flags)
{
	int err;

	err = akcipher_check_key(sock);
	if (err)
		return err;

	return akcipher_sendpage(sock, page, offset, size, flags);
}

static int akcipher_recvmsg_nokey(struct socket *sock, struct msghdr *msg,
				  size_t ignored, int flags)
{
	int err;

	err = akcipher_check_key(sock);
	if (err)
		return err;

	return akcipher_recvmsg(sock, msg, ignored, flags);
}

static struct proto_ops algif_akcipher_ops_nokey = {
	.family		=	PF_ALG,

	.connect	=	sock_no_connect,
	.socketpair	=	sock_no_socketpair,
	.getname	=	sock_no_getname,
	.ioctl		=	sock_no_ioctl,
	.listen		=	sock_no_listen,
	.shutdown	=	sock_no_shutdown,
	.getsockopt	=	sock_no_getsockopt,
	.mmap		=	sock_no_mmap,
	.bind		=	sock_no_bind,
	.accept		=	sock_no_accept,
	.setsockopt	=	sock_no_setsockopt,

	.release	=	af_alg_release,
	.sendmsg	=	akcipher_sendmsg_nokey,
	.sendpage	=	akcipher_sendpage_nokey,
	.recvmsg	=	akcipher_recvmsg_nokey,
	.poll		=	akcipher_poll,
};

static void *akcipher_bind(const char *name, u32 type, u32 mask)
{
	struct akcipher_tfm *tfm;
	struct crypto_akcipher *akcipher;

	tfm = kzalloc(sizeof(*tfm), GFP_KERNEL);
	if (!tfm)
		return ERR_PTR(-ENOMEM);

	akcipher = crypto_alloc_akcipher(name, type, mask);
	if (IS_ERR(akcipher)) {
		kfree(tfm);
		return ERR_CAST(akcipher);
	}

	tfm->akcipher = akcipher;
	return tfm;
}

static void akcipher_release(void *private)
{
	struct akcipher_tfm *tfm = private;
	struct crypto_akcipher *akcipher = tfm->akcipher;

	crypto_free_akcipher(akcipher);
	kfree(tfm);
}

static int akcipher_setkeyid(void *private, const u8 *key, unsigned int keylen)
{
	struct akcipher_tfm *tfm = private;
	struct key *akey;
	u32 keyid = *((u32 *)key);
	int err = -ENOKEY;

	/* Store the key id and verify that a key with the given id is present.
	 * The actual key will be acquired in the accept_parent function
	 */
	sprintf(tfm->keyid, "id:%08x", keyid);
	akey = request_key(&key_type_asymmetric, tfm->keyid, NULL);
	if (IS_ERR(key))
		goto out;

	tfm->has_key = true;
	key_put(akey);
out:
	return err;
}

static int akcipher_setprivkey(void *private, const u8 *key,
			       unsigned int keylen)
{
	struct akcipher_tfm *tfm = private;
	struct crypto_akcipher *akcipher = tfm->akcipher;
	int err;

	err = crypto_akcipher_set_priv_key(akcipher, key, keylen);
	tfm->has_key = !err;
	return err;
}

static int akcipher_setpubkey(void *private, const u8 *key, unsigned int keylen)
{
	struct akcipher_tfm *tfm = private;
	struct crypto_akcipher *akcipher = tfm->akcipher;
	int err;

	err = crypto_akcipher_set_pub_key(akcipher, key, keylen);
	tfm->has_key = !err;
	return err;
}

static void akcipher_sock_destruct(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_ctx *ctx = ask->private;

	akcipher_put_sgl(sk);
	sock_kfree_s(sk, ctx, ctx->len);
	af_alg_release_parent(sk);
	if (ctx->key)
		key_put(ctx->key);
}

static int akcipher_accept_parent_nokey(void *private, struct sock *sk)
{
	struct akcipher_ctx *ctx;
	struct alg_sock *ask = alg_sk(sk);
	struct akcipher_tfm *tfm = private;
	struct crypto_akcipher *akcipher = tfm->akcipher;
	struct key *key;
	unsigned int len = sizeof(*ctx) + crypto_akcipher_reqsize(akcipher);

	ctx = sock_kmalloc(sk, len, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	memset(ctx, 0, len);

	ctx->len = len;
	ctx->used = 0;
	ctx->more = 0;
	ctx->merge = 0;
	ctx->op = 0;
	ctx->tsgl.cur = 0;
	af_alg_init_completion(&ctx->completion);
	sg_init_table(ctx->tsgl.sg, ALG_MAX_PAGES);

	if (strlen(tfm->keyid)) {
		key = request_key(&key_type_asymmetric, tfm->keyid, NULL);
		if (IS_ERR(key)) {
			sock_kfree_s(sk, ctx, len);
			return -ENOKEY;
		}

		ctx->key = key;
		memset(tfm->keyid, '\0', sizeof(tfm->keyid));
	}
	akcipher_request_set_tfm(&ctx->req, akcipher);
	akcipher_request_set_callback(&ctx->req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      af_alg_complete, &ctx->completion);
	ask->private = ctx;

	sk->sk_destruct = akcipher_sock_destruct;

	return 0;
}

static int akcipher_accept_parent(void *private, struct sock *sk)
{
	struct akcipher_tfm *tfm = private;

	if (!tfm->has_key)
		return -ENOKEY;

	return akcipher_accept_parent_nokey(private, sk);
}

static const struct af_alg_type algif_type_akcipher = {
	.bind		=	akcipher_bind,
	.release	=	akcipher_release,
	.setkey		=	akcipher_setprivkey,
	.setpubkey	=	akcipher_setpubkey,
	.setkeyid	=	akcipher_setkeyid,
	.accept		=	akcipher_accept_parent,
	.accept_nokey	=	akcipher_accept_parent_nokey,
	.ops		=	&algif_akcipher_ops,
	.ops_nokey	=	&algif_akcipher_ops_nokey,
	.name		=	"akcipher",
	.owner		=	THIS_MODULE
};

static int __init algif_akcipher_init(void)
{
	return af_alg_register_type(&algif_type_akcipher);
}

static void __exit algif_akcipher_exit(void)
{
	int err = af_alg_unregister_type(&algif_type_akcipher);

	WARN_ON(err);
}

module_init(algif_akcipher_init);
module_exit(algif_akcipher_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Asymmetric kernel crypto API user space interface");
