/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef VAS_H
#define VAS_H

#define VAS_RX_FIFO_SIZE_MAX	(8 << 20)	/* 8MB */
/*
 * Co-processor Engine type.
 */
enum vas_cop_type {
	VAS_COP_TYPE_FAULT,
	VAS_COP_TYPE_842,
	VAS_COP_TYPE_842_HIPRI,
	VAS_COP_TYPE_GZIP,
	VAS_COP_TYPE_GZIP_HIPRI,
	VAS_COP_TYPE_MAX,
};

/*
 * Threshold Control Mode: Have paste operation fail if the number of
 * requests in receive FIFO exceeds a threshold.
 *
 * NOTE: No special error code yet if paste is rejected because of these
 *	 limits. So users can't distinguish between this and other errors.
 */
enum vas_thresh_ctl {
	VAS_THRESH_DISABLED,
	VAS_THRESH_FIFO_GT_HALF_FULL,
	VAS_THRESH_FIFO_GT_QTR_FULL,
	VAS_THRESH_FIFO_GT_EIGHTH_FULL,
};

/*
 * Receive window attributes specified by the (in-kernel) owner of window.
 */
struct vas_rx_win_attr {
	void *rx_fifo;
	int rx_fifo_size;
	int wcreds_max;

	bool pin_win;
	bool rej_no_credit;
	bool tx_wcred_mode;
	bool rx_wcred_mode;
	bool tx_win_ord_mode;
	bool rx_win_ord_mode;
	bool data_stamp;
	bool nx_win;
	bool fault_win;
	bool notify_disable;
	bool intr_disable;
	bool notify_early;

	int lnotify_lpid;
	int lnotify_pid;
	int lnotify_tid;
	int pswid;

	enum vas_thresh_ctl tc_mode;
};

/*
 * Open a VAS receive window for the instance of VAS identified by @vasid
 * Use @attr to initialize the attributes of the window.
 *
 * Return a handle to the window or ERR_PTR() on error.
 */
struct vas_window *vas_rx_win_open(int vasid, enum vas_cop_type cop,
			struct vas_rx_win_attr *attr);


/*
 * Get/Set bit fields
 */
#define GET_FIELD(m, v)		(((v) & (m)) >> MASK_LSH(m))
#define MASK_LSH(m)		(__builtin_ffsl(m) - 1)
#define SET_FIELD(m, v, val)	\
		(((v) & ~(m)) | ((((typeof(v))(val)) << MASK_LSH(m)) & (m)))

#endif
