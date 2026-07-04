// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __SERVICES_PROC_H__
#define __SERVICES_PROC_H__

#include <linux/seq_file.h>
#include "img_defs.h"

struct pvr_proc_dir_entry;

#define PVR_PROC_SEQ_START_TOKEN (void*)1
typedef void* (pvr_next_proc_seq_t)(struct seq_file *,void*,loff_t);
typedef void* (pvr_off2element_proc_seq_t)(struct seq_file *, loff_t);
typedef void (pvr_show_proc_seq_t)(struct seq_file *,void*);
typedef void (pvr_startstop_proc_seq_t)(struct seq_file *, IMG_BOOL start);

typedef	int (pvr_proc_write_t)(struct file *file, const char __user *buffer,
                               unsigned long count, void *data);

IMG_INT CreateProcEntries(void);
void RemoveProcEntries(void);

struct pvr_proc_dir_entry* CreateProcReadEntrySeq(const IMG_CHAR* name,
                                                  IMG_VOID* data,
                                                  pvr_next_proc_seq_t next_handler,
                                                  pvr_show_proc_seq_t show_handler,
                                                  pvr_off2element_proc_seq_t off2element_handler,
                                                  pvr_startstop_proc_seq_t startstop_handler);

struct pvr_proc_dir_entry* CreateProcEntrySeq(const IMG_CHAR* name,
                                              IMG_VOID* data,
                                              pvr_next_proc_seq_t next_handler,
                                              pvr_show_proc_seq_t show_handler,
                                              pvr_off2element_proc_seq_t off2element_handler,
                                              pvr_startstop_proc_seq_t startstop_handler,
                                              pvr_proc_write_t whandler);

struct pvr_proc_dir_entry* CreatePerProcessProcEntrySeq(const IMG_CHAR* name,
                                                        IMG_VOID* data,
                                                        pvr_next_proc_seq_t next_handler,
                                                        pvr_show_proc_seq_t show_handler,
                                                        pvr_off2element_proc_seq_t off2element_handler,
                                                        pvr_startstop_proc_seq_t startstop_handler,
                                                        pvr_proc_write_t whandler);

void RemoveProcEntrySeq(struct pvr_proc_dir_entry* proc_entry);
void RemovePerProcessProcEntrySeq(struct pvr_proc_dir_entry* proc_entry);

void *PVRProcGetData(struct pvr_proc_dir_entry* ppde);

#endif
