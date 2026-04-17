# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the CFS filesystem
#
# CFS - Ceph File System Simple
#

# CFS core module
obj-$(CONFIG_CFS_FS) += cfs.o

# CFS source files
cfs-y := super.o inode.o file.o addr.o dir.o rados.o netfs.o

# CFS dependencies
cfs-$(CONFIG_CEPH_LIB) += rados.o