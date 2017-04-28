/*
   Unix SMB/CIFS implementation.

   test suite for SMB2 ioctl operations

   Copyright (C) David Disseldorp 2011-2015

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "librpc/gen_ndr/security.h"
#include "libcli/smb2/smb2.h"
#include "libcli/smb2/smb2_calls.h"
#include "torture/torture.h"
#include "torture/smb2/proto.h"
#include "../libcli/smb/smbXcli_base.h"
#include "librpc/gen_ndr/ndr_ioctl.h"

#define FNAME	"testfsctl.dat"
#define FNAME2	"testfsctl2.dat"
#define DNAME	"testfsctl_dir"

/*
   basic testing of SMB2 shadow copy calls
*/
static bool test_ioctl_get_shadow_copy(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	struct smb2_handle h;
	uint8_t buf[100];
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);

	smb2_util_unlink(tree, FNAME);

	status = torture_smb2_testfile(tree, FNAME, &h);
	torture_assert_ntstatus_ok(torture, status, "create write");

	ZERO_ARRAY(buf);
	status = smb2_util_write(tree, h, buf, 0, ARRAY_SIZE(buf));
	torture_assert_ntstatus_ok(torture, status, "write");

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = h;
	ioctl.smb2.in.function = FSCTL_SRV_ENUM_SNAPS;
	ioctl.smb2.in.max_response_size = 16;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_SUPPORTED)
	 || NT_STATUS_EQUAL(status, NT_STATUS_INVALID_DEVICE_REQUEST)) {
		torture_skip(torture, "FSCTL_SRV_ENUM_SNAPS not supported\n");
	}
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_ENUM_SNAPS");

	return true;
}

/*
   basic testing of the SMB2 server side copy ioctls
*/
static bool test_ioctl_req_resume_key(struct torture_context *torture,
				      struct smb2_tree *tree)
{
	struct smb2_handle h;
	uint8_t buf[100];
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct req_resume_key_rsp res_key;
	enum ndr_err_code ndr_ret;

	smb2_util_unlink(tree, FNAME);

	status = torture_smb2_testfile(tree, FNAME, &h);
	torture_assert_ntstatus_ok(torture, status, "create write");

	ZERO_ARRAY(buf);
	status = smb2_util_write(tree, h, buf, 0, ARRAY_SIZE(buf));
	torture_assert_ntstatus_ok(torture, status, "write");

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = h;
	ioctl.smb2.in.function = FSCTL_SRV_REQUEST_RESUME_KEY;
	ioctl.smb2.in.max_response_size = 32;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_REQUEST_RESUME_KEY");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx, &res_key,
			(ndr_pull_flags_fn_t)ndr_pull_req_resume_key_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_req_resume_key_rsp");

	ndr_print_debug((ndr_print_fn_t)ndr_print_req_resume_key_rsp, "yo", &res_key);

	talloc_free(tmp_ctx);
	return true;
}

static uint64_t patt_hash(uint64_t off)
{
	return off;
}

static bool write_pattern(struct torture_context *torture,
			  struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
			  struct smb2_handle h, uint64_t off, uint64_t len,
			  uint64_t patt_off)
{
	NTSTATUS status;
	uint64_t i;
	uint8_t *buf;
	uint64_t io_sz = MIN(1024 * 64, len);

	if (len == 0) {
		return true;
	}

	torture_assert(torture, (len % 8) == 0, "invalid write len");

	buf = talloc_zero_size(mem_ctx, io_sz);
	torture_assert(torture, (buf != NULL), "no memory for file data buf");

	while (len > 0) {
		for (i = 0; i <= io_sz - 8; i += 8) {
			SBVAL(buf, i, patt_hash(patt_off));
			patt_off += 8;
		}

		status = smb2_util_write(tree, h,
					 buf, off, io_sz);
		torture_assert_ntstatus_ok(torture, status, "file write");

		len -= io_sz;
		off += io_sz;
	}

	talloc_free(buf);

	return true;
}

static bool check_pattern(struct torture_context *torture,
			  struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
			  struct smb2_handle h, uint64_t off, uint64_t len,
			  uint64_t patt_off)
{
	if (len == 0) {
		return true;
	}

	torture_assert(torture, (len % 8) == 0, "invalid read len");

	while (len > 0) {
		uint64_t i;
		struct smb2_read r;
		NTSTATUS status;
		uint64_t io_sz = MIN(1024 * 64, len);

		ZERO_STRUCT(r);
		r.in.file.handle = h;
		r.in.length      = io_sz;
		r.in.offset      = off;
		status = smb2_read(tree, mem_ctx, &r);
		torture_assert_ntstatus_ok(torture, status, "read");

		torture_assert_u64_equal(torture, r.out.data.length, io_sz,
					 "read data len mismatch");

		for (i = 0; i <= io_sz - 8; i += 8, patt_off += 8) {
			uint64_t data = BVAL(r.out.data.data, i);
			torture_assert_u64_equal(torture, data, patt_hash(patt_off),
						 talloc_asprintf(torture, "read data "
								 "pattern bad at %llu\n",
								 (unsigned long long)off + i));
		}
		talloc_free(r.out.data.data);
		len -= io_sz;
		off += io_sz;
	}

	return true;
}

static bool check_zero(struct torture_context *torture,
		       struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
		       struct smb2_handle h, uint64_t off, uint64_t len)
{
	uint64_t i;
	struct smb2_read r;
	NTSTATUS status;

	if (len == 0) {
		return true;
	}

	ZERO_STRUCT(r);
	r.in.file.handle = h;
	r.in.length      = len;
	r.in.offset      = off;
	status = smb2_read(tree, mem_ctx, &r);
	torture_assert_ntstatus_ok(torture, status, "read");

	torture_assert_u64_equal(torture, r.out.data.length, len,
				 "read data len mismatch");

	for (i = 0; i <= len - 8; i += 8) {
		uint64_t data = BVAL(r.out.data.data, i);
		torture_assert_u64_equal(torture, data, 0,
					 talloc_asprintf(mem_ctx, "read zero "
							 "bad at %llu\n",
							 (unsigned long long)i));
	}

	talloc_free(r.out.data.data);
	return true;
}

static bool test_setup_open(struct torture_context *torture,
			    struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
			    const char *fname,
			    struct smb2_handle *fh,
			    uint32_t desired_access,
			    uint32_t file_attributes)
{
	struct smb2_create io;
	NTSTATUS status;

	ZERO_STRUCT(io);
	io.in.desired_access = desired_access;
	io.in.file_attributes = file_attributes;
	io.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io.in.share_access =
		NTCREATEX_SHARE_ACCESS_DELETE|
		NTCREATEX_SHARE_ACCESS_READ|
		NTCREATEX_SHARE_ACCESS_WRITE;
	if (file_attributes & FILE_ATTRIBUTE_DIRECTORY) {
		io.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	}
	io.in.fname = fname;

	status = smb2_create(tree, mem_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "file create");

	*fh = io.out.file.handle;

	return true;
}

static bool test_setup_create_fill(struct torture_context *torture,
				   struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
				   const char *fname,
				   struct smb2_handle *fh,
				   uint64_t size,
				   uint32_t desired_access,
				   uint32_t file_attributes)
{
	bool ok;

	smb2_util_unlink(tree, fname);

	ok = test_setup_open(torture, tree, mem_ctx,
			     fname,
			     fh,
			     desired_access,
			     file_attributes);
	torture_assert(torture, ok, "file open");

	if (size > 0) {
		ok = write_pattern(torture, tree, mem_ctx, *fh, 0, size, 0);
		torture_assert(torture, ok, "write pattern");
	}
	return true;
}

static bool test_setup_copy_chunk(struct torture_context *torture,
				  struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
				  uint32_t nchunks,
				  struct smb2_handle *src_h,
				  uint64_t src_size,
				  uint32_t src_desired_access,
				  struct smb2_handle *dest_h,
				  uint64_t dest_size,
				  uint32_t dest_desired_access,
				  struct srv_copychunk_copy *cc_copy,
				  union smb_ioctl *ioctl)
{
	struct req_resume_key_rsp res_key;
	bool ok;
	NTSTATUS status;
	enum ndr_err_code ndr_ret;

	ok = test_setup_create_fill(torture, tree, mem_ctx, FNAME,
				    src_h, src_size, src_desired_access,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "src file create fill");

	ok = test_setup_create_fill(torture, tree, mem_ctx, FNAME2,
				    dest_h, dest_size, dest_desired_access,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "dest file create fill");

	ZERO_STRUCTPN(ioctl);
	ioctl->smb2.level = RAW_IOCTL_SMB2;
	ioctl->smb2.in.file.handle = *src_h;
	ioctl->smb2.in.function = FSCTL_SRV_REQUEST_RESUME_KEY;
	/* Allow for Key + ContextLength + Context */
	ioctl->smb2.in.max_response_size = 32;
	ioctl->smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, mem_ctx, &ioctl->smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_REQUEST_RESUME_KEY");

	ndr_ret = ndr_pull_struct_blob(&ioctl->smb2.out.out, mem_ctx, &res_key,
			(ndr_pull_flags_fn_t)ndr_pull_req_resume_key_rsp);

	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_req_resume_key_rsp");

	ZERO_STRUCTPN(ioctl);
	ioctl->smb2.level = RAW_IOCTL_SMB2;
	ioctl->smb2.in.file.handle = *dest_h;
	ioctl->smb2.in.function = FSCTL_SRV_COPYCHUNK;
	ioctl->smb2.in.max_response_size = sizeof(struct srv_copychunk_rsp);
	ioctl->smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	ZERO_STRUCTPN(cc_copy);
	memcpy(cc_copy->source_key, res_key.resume_key, ARRAY_SIZE(cc_copy->source_key));
	cc_copy->chunk_count = nchunks;
	cc_copy->chunks = talloc_zero_array(mem_ctx, struct srv_copychunk, nchunks);
	torture_assert(torture, (cc_copy->chunks != NULL), "no memory for chunks");

	return true;
}


static bool check_copy_chunk_rsp(struct torture_context *torture,
				 struct srv_copychunk_rsp *cc_rsp,
				 uint32_t ex_chunks_written,
				 uint32_t ex_chunk_bytes_written,
				 uint32_t ex_total_bytes_written)
{
	torture_assert_int_equal(torture, cc_rsp->chunks_written,
				 ex_chunks_written, "num chunks");
	torture_assert_int_equal(torture, cc_rsp->chunk_bytes_written,
				 ex_chunk_bytes_written, "chunk bytes written");
	torture_assert_int_equal(torture, cc_rsp->total_bytes_written,
				 ex_total_bytes_written, "chunk total bytes");
	return true;
}

static bool test_ioctl_copy_chunk_simple(struct torture_context *torture,
					 struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* copy all src file data (via a single chunk desc) */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  4096); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_multi(struct torture_context *torture,
					struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   2, /* chunks */
				   &src_h, 8192, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* copy all src file data via two chunks */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	cc_copy.chunks[1].source_off = 4096;
	cc_copy.chunks[1].target_off = 4096;
	cc_copy.chunks[1].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  2,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  8192);	/* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_tiny(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   2, /* chunks */
				   &src_h, 96, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* copy all src file data via two chunks, sub block size chunks */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 48;

	cc_copy.chunks[1].source_off = 48;
	cc_copy.chunks[1].target_off = 48;
	cc_copy.chunks[1].length = 48;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  2,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  96);	/* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 96, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_over(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   2, /* chunks */
				   &src_h, 8192, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 4096, /* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* first chunk overwrites existing dest data */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	/* second chunk overwrites the first */
	cc_copy.chunks[1].source_off = 4096;
	cc_copy.chunks[1].target_off = 0;
	cc_copy.chunks[1].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  2,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  8192); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 4096, 4096);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_append(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   2, /* chunks */
				   &src_h, 4096, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	/* second chunk appends the same data to the first */
	cc_copy.chunks[1].source_off = 0;
	cc_copy.chunks[1].target_off = 4096;
	cc_copy.chunks[1].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  2,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  8192); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 4096, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_limits(struct torture_context *torture,
					 struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* chunks */
				   &src_h, 4096, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* send huge chunk length request */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = UINT_MAX;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret, "marshalling request");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER,
				      "bad oversize chunk response");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret, "unmarshalling response");

	torture_comment(torture, "limit max chunks, got %u\n",
			cc_rsp.chunks_written);
	torture_comment(torture, "limit max chunk len, got %u\n",
			cc_rsp.chunk_bytes_written);
	torture_comment(torture, "limit max total bytes, got %u\n",
			cc_rsp.total_bytes_written);

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_src_lck(struct torture_context *torture,
					  struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle src_h2;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;
	struct smb2_lock lck;
	struct smb2_lock_element el[1];

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* chunks */
				   &src_h, 4096, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	/* open and lock the copychunk src file */
	status = torture_smb2_testfile(tree, FNAME, &src_h2);
	torture_assert_ntstatus_ok(torture, status, "2nd src open");

	lck.in.lock_count	= 0x0001;
	lck.in.lock_sequence	= 0x00000000;
	lck.in.file.handle	= src_h2;
	lck.in.locks		= el;
	el[0].offset		= cc_copy.chunks[0].source_off;
	el[0].length		= cc_copy.chunks[0].length;
	el[0].reserved		= 0;
	el[0].flags		= SMB2_LOCK_FLAG_EXCLUSIVE;

	status = smb2_lock(tree, &lck);
	torture_assert_ntstatus_ok(torture, status, "lock");

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	/*
	 * 2k12 & Samba return lock_conflict, Windows 7 & 2k8 return success...
	 *
	 * Edgar Olougouna @ MS wrote:
	 * Regarding the FSCTL_SRV_COPYCHUNK and STATUS_FILE_LOCK_CONFLICT
	 * discrepancy observed between Windows versions, we confirm that the
	 * behavior change is expected.
	 *
	 * CopyChunk in Windows Server 2012 use regular Readfile/Writefile APIs
	 * to move the chunks from the source to the destination.
	 * These ReadFile/WriteFile APIs go through the byte-range lock checks,
	 * and this explains the observed STATUS_FILE_LOCK_CONFLICT error.
	 *
	 * Prior to Windows Server 2012, CopyChunk used mapped sections to move
	 * the data. And byte range locks are not enforced on mapped I/O, and
	 * this explains the STATUS_SUCCESS observed on Windows Server 2008 R2.
	 */
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_FILE_LOCK_CONFLICT,
				      "FSCTL_SRV_COPYCHUNK locked");

	/* should get cc response data with the lock conflict status */
	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");
	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  0,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  0);	/* total bytes written */

	lck.in.lock_count	= 0x0001;
	lck.in.lock_sequence	= 0x00000001;
	lck.in.file.handle	= src_h2;
	lck.in.locks		= el;
	el[0].offset		= cc_copy.chunks[0].source_off;
	el[0].length		= cc_copy.chunks[0].length;
	el[0].reserved		= 0;
	el[0].flags		= SMB2_LOCK_FLAG_UNLOCK;
	status = smb2_lock(tree, &lck);
	torture_assert_ntstatus_ok(torture, status, "unlock");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_COPYCHUNK unlocked");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  4096); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h2);
	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_dest_lck(struct torture_context *torture,
					   struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	struct smb2_handle dest_h2;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;
	struct smb2_lock lck;
	struct smb2_lock_element el[1];

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* chunks */
				   &src_h, 4096, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 4096,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	/* open and lock the copychunk dest file */
	status = torture_smb2_testfile(tree, FNAME2, &dest_h2);
	torture_assert_ntstatus_ok(torture, status, "2nd src open");

	lck.in.lock_count	= 0x0001;
	lck.in.lock_sequence	= 0x00000000;
	lck.in.file.handle	= dest_h2;
	lck.in.locks		= el;
	el[0].offset		= cc_copy.chunks[0].target_off;
	el[0].length		= cc_copy.chunks[0].length;
	el[0].reserved		= 0;
	el[0].flags		= SMB2_LOCK_FLAG_EXCLUSIVE;

	status = smb2_lock(tree, &lck);
	torture_assert_ntstatus_ok(torture, status, "lock");

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_FILE_LOCK_CONFLICT,
				      "FSCTL_SRV_COPYCHUNK locked");

	lck.in.lock_count	= 0x0001;
	lck.in.lock_sequence	= 0x00000001;
	lck.in.file.handle	= dest_h2;
	lck.in.locks		= el;
	el[0].offset		= cc_copy.chunks[0].target_off;
	el[0].length		= cc_copy.chunks[0].length;
	el[0].reserved		= 0;
	el[0].flags		= SMB2_LOCK_FLAG_UNLOCK;
	status = smb2_lock(tree, &lck);
	torture_assert_ntstatus_ok(torture, status, "unlock");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_COPYCHUNK unlocked");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  4096); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, dest_h2);
	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_bad_key(struct torture_context *torture,
					  struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1,
				   &src_h, 4096,
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* overwrite the resume key with a bogus value */
	memcpy(cc_copy.source_key, "deadbeefdeadbeefdeadbeef", 24);

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	/* Server 2k12 returns NT_STATUS_OBJECT_NAME_NOT_FOUND */
	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_OBJECT_NAME_NOT_FOUND,
				      "FSCTL_SRV_COPYCHUNK");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_src_is_dest(struct torture_context *torture,
					      struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1,
				   &src_h, 8192,
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* the source is also the destination */
	ioctl.smb2.in.file.handle = src_h;

	/* non-overlapping */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 4096;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  4096); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, src_h, 0, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}
	ok = check_pattern(torture, tree, tmp_ctx, src_h, 4096, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

/*
 * Test a single-chunk copychunk request, where the source and target ranges
 * overlap, and the SourceKey refers to the same target file. E.g:
 *
 * Initial State
 * -------------
 * 	File:		src_and_dest
 * 	Offset:		0123456789
 * 	Data:		abcdefghij
 *
 * Request
 * -------
 * 	FSCTL_SRV_COPYCHUNK(src_and_dest)
 * 	SourceKey = SRV_REQUEST_RESUME_KEY(src_and_dest)
 * 	ChunkCount = 1
 * 	Chunks[0].SourceOffset = 0
 * 	Chunks[0].TargetOffset = 4
 * 	Chunks[0].Length = 6
 *
 * Resultant State
 * ---------------
 * 	File:		src_and_dest
 * 	Offset:		0123456789
 * 	Data:		abcdabcdef
 *
 * The resultant contents of src_and_dest is dependent on the server's
 * copy algorithm. In the above example, the server uses an IO buffer
 * large enough to hold the entire six-byte source data before writing
 * to TargetOffset. If the server were to use a four-byte IO buffer and
 * started reads/writes from the lowest offset, then the two overlapping
 * bytes in the above example would be overwritten before being read. The
 * resultant file contents would be abcdabcdab.
 *
 * Windows 2008r2 appears to use a 2048 byte copy buffer, overlapping bytes
 * after this offset are written before being read. Windows 2012 on the
 * other hand appears to use a buffer large enough to hold its maximum
 * supported chunk size (1M). Samba currently uses a 64k copy buffer by
 * default (vfs_cc_state.buf).
 *
 * This test uses an 8-byte overlap at 2040-2048, so that it passes against
 * Windows 2008r2, 2012 and Samba servers. Note, 2008GM fails, as it appears
 * to use a different copy algorithm to 2008r2.
 */
static bool
test_ioctl_copy_chunk_src_is_dest_overlap(struct torture_context *torture,
					  struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	/* exceed the vfs_default copy buffer */
	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1,
				   &src_h, 2048 * 2,
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* the source is also the destination */
	ioctl.smb2.in.file.handle = src_h;

	/* 8 bytes overlap between source and target ranges */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 2048 - 8;
	cc_copy.chunks[0].length = 2048;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  2048); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, src_h, 0, 2048 - 8, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}
	ok = check_pattern(torture, tree, tmp_ctx, src_h, 2048 - 8, 2048, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_bad_access(struct torture_context *torture,
					     struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	enum ndr_err_code ndr_ret;
	bool ok;

	/* no read permission on src */
	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_WRITE,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "FSCTL_SRV_COPYCHUNK");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);

	/* no write permission on dest */
	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   (SEC_RIGHTS_FILE_READ
				    | SEC_RIGHTS_FILE_EXECUTE),
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "FSCTL_SRV_COPYCHUNK");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);

	/* no read permission on dest */
	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   (SEC_RIGHTS_FILE_WRITE
				    | SEC_RIGHTS_FILE_EXECUTE),
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	/*
	 * FSCTL_SRV_COPYCHUNK requires read permission on dest,
	 * FSCTL_SRV_COPYCHUNK_WRITE on the other hand does not.
	 */
	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "FSCTL_SRV_COPYCHUNK");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);

	return true;
}

static bool test_ioctl_copy_chunk_write_access(struct torture_context *torture,
					       struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	enum ndr_err_code ndr_ret;
	bool ok;

	/* no read permission on dest with FSCTL_SRV_COPYCHUNK_WRITE */
	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   (SEC_RIGHTS_FILE_WRITE
				    | SEC_RIGHTS_FILE_EXECUTE),
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	ioctl.smb2.in.function = FSCTL_SRV_COPYCHUNK_WRITE;
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_COPYCHUNK_WRITE");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);

	return true;
}

static bool test_ioctl_copy_chunk_src_exceed(struct torture_context *torture,
					     struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* Request copy where off + length exceeds size of src */
	cc_copy.chunks[0].source_off = 1024;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_VIEW_SIZE,
				      "FSCTL_SRV_COPYCHUNK oversize");

	/* Request copy where length exceeds size of src */
	cc_copy.chunks[0].source_off = 1024;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 3072;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_SRV_COPYCHUNK just right");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  3072); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 3072, 1024);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool
test_ioctl_copy_chunk_src_exceed_multi(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   2, /* 2 chunks */
				   &src_h, 8192, /* fill 8192 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* Request copy where off + length exceeds size of src */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;

	cc_copy.chunks[1].source_off = 4096;
	cc_copy.chunks[1].target_off = 4096;
	cc_copy.chunks[1].length = 8192;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_VIEW_SIZE,
				      "FSCTL_SRV_COPYCHUNK oversize");
	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret, "unmarshalling response");

	/* first chunk should still be written */
	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  4096); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}
	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 0, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_sparse_dest(struct torture_context *torture,
					      struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	struct smb2_read r;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;
	int i;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* copy all src file data (via a single chunk desc) */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 4096;
	cc_copy.chunks[0].length = 4096;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  4096); /* total bytes written */
	if (!ok) {
		torture_fail(torture, "bad copy chunk response data");
	}

	/* check for zeros in first 4k */
	ZERO_STRUCT(r);
	r.in.file.handle = dest_h;
	r.in.length      = 4096;
	r.in.offset      = 0;
	status = smb2_read(tree, tmp_ctx, &r);
	torture_assert_ntstatus_ok(torture, status, "read");

	torture_assert_u64_equal(torture, r.out.data.length, 4096,
				 "read data len mismatch");

	for (i = 0; i < 4096; i++) {
		torture_assert(torture, (r.out.data.data[i] == 0),
			       "sparse did not pass class");
	}

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, 4096, 4096, 0);
	if (!ok) {
		torture_fail(torture, "inconsistent file data");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

/*
 * set the ioctl MaxOutputResponse size to less than
 * sizeof(struct srv_copychunk_rsp)
 */
static bool test_ioctl_copy_chunk_max_output_sz(struct torture_context *torture,
						struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 4096;
	/* req is valid, but use undersize max_response_size */
	ioctl.smb2.in.max_response_size = sizeof(struct srv_copychunk_rsp) - 1;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER,
				      "FSCTL_SRV_COPYCHUNK");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_copy_chunk_zero_length(struct torture_context *torture,
					      struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	union smb_ioctl ioctl;
	union smb_fileinfo q;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* 1 chunk */
				   &src_h, 4096, /* fill 4096 byte src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* 0 byte dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	if (!ok) {
		torture_fail(torture, "setup copy chunk error");
	}

	/* zero length server-side copy (via a single chunk desc) */
	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = 0;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER,
				      "bad zero-length chunk response");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret, "unmarshalling response");

	ZERO_STRUCT(q);
	q.all_info2.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
	q.all_info2.in.file.handle = dest_h;
	status = smb2_getinfo_file(tree, torture, &q);
	torture_assert_ntstatus_ok(torture, status, "getinfo");

	torture_assert_int_equal(torture, q.all_info2.out.size, 0,
				 "size after zero len clone");

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static NTSTATUS test_ioctl_compress_fs_supported(struct torture_context *torture,
						 struct smb2_tree *tree,
						 TALLOC_CTX *mem_ctx,
						 struct smb2_handle *fh,
						 bool *compress_support)
{
	NTSTATUS status;
	union smb_fsinfo info;

	ZERO_STRUCT(info);
	info.generic.level = RAW_QFS_ATTRIBUTE_INFORMATION;
	info.generic.handle = *fh;
	status = smb2_getinfo_fs(tree, tree, &info);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (info.attribute_info.out.fs_attr & FILE_FILE_COMPRESSION) {
		*compress_support = true;
	} else {
		*compress_support = false;
	}
	return NT_STATUS_OK;
}

static NTSTATUS test_ioctl_compress_get(struct torture_context *torture,
					TALLOC_CTX *mem_ctx,
					struct smb2_tree *tree,
					struct smb2_handle fh,
					uint16_t *_compression_fmt)
{
	union smb_ioctl ioctl;
	struct compression_state cmpr_state;
	enum ndr_err_code ndr_ret;
	NTSTATUS status;

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_GET_COMPRESSION;
	ioctl.smb2.in.max_response_size = sizeof(struct compression_state);
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, mem_ctx, &ioctl.smb2);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, mem_ctx,
				       &cmpr_state,
			(ndr_pull_flags_fn_t)ndr_pull_compression_state);

	if (ndr_ret != NDR_ERR_SUCCESS) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	*_compression_fmt = cmpr_state.format;
	return NT_STATUS_OK;
}

static NTSTATUS test_ioctl_compress_set(struct torture_context *torture,
					TALLOC_CTX *mem_ctx,
					struct smb2_tree *tree,
					struct smb2_handle fh,
					uint16_t compression_fmt)
{
	union smb_ioctl ioctl;
	struct compression_state cmpr_state;
	enum ndr_err_code ndr_ret;
	NTSTATUS status;

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_COMPRESSION;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	cmpr_state.format = compression_fmt;
	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, mem_ctx,
				       &cmpr_state,
			(ndr_push_flags_fn_t)ndr_push_compression_state);
	if (ndr_ret != NDR_ERR_SUCCESS) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	status = smb2_ioctl(tree, mem_ctx, &ioctl.smb2);
	return status;
}

static bool test_ioctl_compress_file_flag(struct torture_context *torture,
					    struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint16_t compression_fmt;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "FS compression not supported\n");
	}

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "initial compression state not NONE");

	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 COMPRESSION_FORMAT_DEFAULT);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_LZNT1),
		       "invalid compression state after set");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_compress_dir_inherit(struct torture_context *torture,
					    struct smb2_tree *tree)
{
	struct smb2_handle dirh;
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	uint16_t compression_fmt;
	bool ok;
	char path_buf[PATH_MAX];

	smb2_deltree(tree, DNAME);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    DNAME, &dirh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_DIRECTORY);
	torture_assert(torture, ok, "setup compression directory");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &dirh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, dirh);
		smb2_deltree(tree, DNAME);
		torture_skip(torture, "FS compression not supported\n");
	}

	/* set compression on parent dir, then check for inheritance */
	status = test_ioctl_compress_set(torture, tmp_ctx, tree, dirh,
					 COMPRESSION_FORMAT_LZNT1);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, dirh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_LZNT1),
		       "invalid compression state after set");

	snprintf(path_buf, PATH_MAX, "%s\\%s", DNAME, FNAME);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    path_buf, &fh, 4096, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_LZNT1),
		       "compression attr not inherited by new file");

	/* check compressed data is consistent */
	ok = check_pattern(torture, tree, tmp_ctx, fh, 0, 4096, 0);

	/* disable dir compression attr, file should remain compressed */
	status = test_ioctl_compress_set(torture, tmp_ctx, tree, dirh,
					 COMPRESSION_FORMAT_NONE);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_LZNT1),
		       "file compression attr removed after dir change");
	smb2_util_close(tree, fh);

	/* new files should no longer inherit compression attr */
	snprintf(path_buf, PATH_MAX, "%s\\%s", DNAME, FNAME2);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    path_buf, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "compression attr present on new file");

	smb2_util_close(tree, fh);
	smb2_util_close(tree, dirh);
	smb2_deltree(tree, DNAME);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_compress_invalid_format(struct torture_context *torture,
					       struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint16_t compression_fmt;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "FS compression not supported\n");
	}

	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 0x0042); /* bogus */
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER,
				      "invalid FSCTL_SET_COMPRESSION");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "initial compression state not NONE");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_compress_invalid_buf(struct torture_context *torture,
					    struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	union smb_ioctl ioctl;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "FS compression not supported\n");
	}

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_GET_COMPRESSION;
	ioctl.smb2.in.max_response_size = 0;	/* no room for rsp data */
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	if (!NT_STATUS_EQUAL(status, NT_STATUS_INVALID_USER_BUFFER)
	 && !NT_STATUS_EQUAL(status, NT_STATUS_INVALID_PARAMETER)) {
		/* neither Server 2k12 nor 2k8r2 response status */
		torture_assert(torture, true,
			       "invalid FSCTL_SET_COMPRESSION");
	}

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_compress_query_file_attr(struct torture_context *torture,
						struct smb2_tree *tree)
{
	struct smb2_handle fh;
	union smb_fileinfo io;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "FS compression not supported\n");
	}

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		((io.all_info2.out.attrib & FILE_ATTRIBUTE_COMPRESSED) == 0),
		       "compression attr before set");

	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 COMPRESSION_FORMAT_DEFAULT);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		       (io.basic_info.out.attrib & FILE_ATTRIBUTE_COMPRESSED),
		       "no compression attr after set");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

/*
 * Specify FILE_ATTRIBUTE_COMPRESSED on creation, Windows does not retain this
 * attribute.
 */
static bool test_ioctl_compress_create_with_attr(struct torture_context *torture,
						 struct smb2_tree *tree)
{
	struct smb2_handle fh2;
	union smb_fileinfo io;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	uint16_t compression_fmt;
	bool ok;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME2, &fh2, 0, SEC_RIGHTS_FILE_ALL,
			(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_COMPRESSED));
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh2,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh2);
		torture_skip(torture, "FS compression not supported\n");
	}

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh2,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "initial compression state not NONE");

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
	io.generic.in.file.handle = fh2;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		((io.all_info2.out.attrib & FILE_ATTRIBUTE_COMPRESSED) == 0),
		       "incorrect compression attr");

	smb2_util_close(tree, fh2);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_compress_inherit_disable(struct torture_context *torture,
						struct smb2_tree *tree)
{
	struct smb2_handle fh;
	struct smb2_handle dirh;
	char path_buf[PATH_MAX];
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint16_t compression_fmt;

	struct smb2_create io;

	smb2_deltree(tree, DNAME);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    DNAME, &dirh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_DIRECTORY);
	torture_assert(torture, ok, "setup compression directory");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &dirh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, dirh);
		smb2_deltree(tree, DNAME);
		torture_skip(torture, "FS compression not supported\n");
	}

	/* set compression on parent dir, then check for inheritance */
	status = test_ioctl_compress_set(torture, tmp_ctx, tree, dirh,
					 COMPRESSION_FORMAT_LZNT1);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, dirh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_LZNT1),
		       "invalid compression state after set");
	smb2_util_close(tree, dirh);

	snprintf(path_buf, PATH_MAX, "%s\\%s", DNAME, FNAME);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    path_buf, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_LZNT1),
		       "compression attr not inherited by new file");
	smb2_util_close(tree, fh);

	snprintf(path_buf, PATH_MAX, "%s\\%s", DNAME, FNAME2);

	/* NO_COMPRESSION option should block inheritance */
	ZERO_STRUCT(io);
	io.in.desired_access = SEC_RIGHTS_FILE_ALL;
	io.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.in.create_options = NTCREATEX_OPTIONS_NO_COMPRESSION;
	io.in.share_access =
		NTCREATEX_SHARE_ACCESS_DELETE|
		NTCREATEX_SHARE_ACCESS_READ|
		NTCREATEX_SHARE_ACCESS_WRITE;
	io.in.fname = path_buf;

	status = smb2_create(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "file create");

	fh = io.out.file.handle;

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "compression attr inherited by NO_COMPRESSION file");
	smb2_util_close(tree, fh);


	snprintf(path_buf, PATH_MAX, "%s\\%s", DNAME, DNAME);
	ZERO_STRUCT(io);
	io.in.desired_access = SEC_RIGHTS_FILE_ALL;
	io.in.file_attributes = FILE_ATTRIBUTE_DIRECTORY;
	io.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.in.create_options = (NTCREATEX_OPTIONS_NO_COMPRESSION
				| NTCREATEX_OPTIONS_DIRECTORY);
	io.in.share_access =
		NTCREATEX_SHARE_ACCESS_DELETE|
		NTCREATEX_SHARE_ACCESS_READ|
		NTCREATEX_SHARE_ACCESS_WRITE;
	io.in.fname = path_buf;

	status = smb2_create(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "dir create");

	dirh = io.out.file.handle;

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, dirh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "compression attr inherited by NO_COMPRESSION dir");
	smb2_util_close(tree, dirh);
	smb2_deltree(tree, DNAME);

	talloc_free(tmp_ctx);
	return true;
}

/* attempting to set compression via SetInfo should not stick */
static bool test_ioctl_compress_set_file_attr(struct torture_context *torture,
					      struct smb2_tree *tree)
{
	struct smb2_handle fh;
	struct smb2_handle dirh;
	union smb_fileinfo io;
	union smb_setfileinfo set_io;
	uint16_t compression_fmt;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "FS compression not supported\n");
	}

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		((io.basic_info.out.attrib & FILE_ATTRIBUTE_COMPRESSED) == 0),
		       "compression attr before set");

	ZERO_STRUCT(set_io);
	set_io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	set_io.basic_info.in.file.handle = fh;
	set_io.basic_info.in.create_time = io.basic_info.out.create_time;
	set_io.basic_info.in.access_time = io.basic_info.out.access_time;
	set_io.basic_info.in.write_time = io.basic_info.out.write_time;
	set_io.basic_info.in.change_time = io.basic_info.out.change_time;
	set_io.basic_info.in.attrib = (io.basic_info.out.attrib
						| FILE_ATTRIBUTE_COMPRESSED);
	status = smb2_setinfo_file(tree, &set_io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_SETINFO_FILE");

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		((io.basic_info.out.attrib & FILE_ATTRIBUTE_COMPRESSED) == 0),
		"compression attr after set");

	smb2_util_close(tree, fh);
	smb2_deltree(tree, DNAME);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    DNAME, &dirh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_DIRECTORY);
	torture_assert(torture, ok, "setup compression directory");

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	io.generic.in.file.handle = dirh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		((io.basic_info.out.attrib & FILE_ATTRIBUTE_COMPRESSED) == 0),
		       "compression attr before set");

	ZERO_STRUCT(set_io);
	set_io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	set_io.basic_info.in.file.handle = dirh;
	set_io.basic_info.in.create_time = io.basic_info.out.create_time;
	set_io.basic_info.in.access_time = io.basic_info.out.access_time;
	set_io.basic_info.in.write_time = io.basic_info.out.write_time;
	set_io.basic_info.in.change_time = io.basic_info.out.change_time;
	set_io.basic_info.in.attrib = (io.basic_info.out.attrib
						| FILE_ATTRIBUTE_COMPRESSED);
	status = smb2_setinfo_file(tree, &set_io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_SETINFO_FILE");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, dirh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");

	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "dir compression set after SetInfo");

	smb2_util_close(tree, dirh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_compress_perms(struct torture_context *torture,
				      struct smb2_tree *tree)
{
	struct smb2_handle fh;
	uint16_t compression_fmt;
	union smb_fileinfo io;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	smb2_util_close(tree, fh);
	if (!ok) {
		torture_skip(torture, "FS compression not supported\n");
	}

	/* attempt get compression without READ_ATTR permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_READ & ~(SEC_FILE_READ_ATTRIBUTE
							| SEC_STD_READ_CONTROL
							| SEC_FILE_READ_EA)),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");
	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "compression set after create");
	smb2_util_close(tree, fh);

	/* set compression without WRITE_ATTR permission should succeed */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_WRITE & ~(SEC_FILE_WRITE_ATTRIBUTE
							| SEC_STD_WRITE_DAC
							| SEC_FILE_WRITE_EA)),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 COMPRESSION_FORMAT_DEFAULT);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");
	smb2_util_close(tree, fh);

	ok = test_setup_open(torture, tree, tmp_ctx,
				    FNAME, &fh, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");
	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		       (io.all_info2.out.attrib & FILE_ATTRIBUTE_COMPRESSED),
		       "incorrect compression attr");
	smb2_util_close(tree, fh);

	/* attempt get compression without READ_DATA permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_READ & ~SEC_FILE_READ_DATA),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");
	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "compression enabled after set");
	smb2_util_close(tree, fh);

	/* attempt get compression with only SYNCHRONIZE permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
				    SEC_STD_SYNCHRONIZE,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_get(torture, tmp_ctx, tree, fh,
					 &compression_fmt);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_GET_COMPRESSION");
	torture_assert(torture, (compression_fmt == COMPRESSION_FORMAT_NONE),
		       "compression not enabled after set");
	smb2_util_close(tree, fh);

	/* attempt to set compression without WRITE_DATA permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_WRITE & (~SEC_FILE_WRITE_DATA)),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 COMPRESSION_FORMAT_DEFAULT);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "FSCTL_SET_COMPRESSION permission");
	smb2_util_close(tree, fh);

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_WRITE & (~SEC_FILE_WRITE_DATA)),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup compression file");

	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 COMPRESSION_FORMAT_NONE);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "FSCTL_SET_COMPRESSION permission");
	smb2_util_close(tree, fh);

	talloc_free(tmp_ctx);
	return true;
}

/*
   basic testing of the SMB2 FSCTL_QUERY_NETWORK_INTERFACE_INFO ioctl
*/
static bool test_ioctl_network_interface_info(struct torture_context *torture,
				      struct smb2_tree *tree)
{
	union smb_ioctl ioctl;
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct fsctl_net_iface_info net_iface;
	enum ndr_err_code ndr_ret;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_MULTI_CHANNEL)) {
		torture_skip(torture, "server doesn't support SMB2_CAP_MULTI_CHANNEL\n");
	}

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	fh.data[0] = UINT64_MAX;
	fh.data[1] = UINT64_MAX;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_QUERY_NETWORK_INTERFACE_INFO;
	ioctl.smb2.in.max_response_size = 0x10000; /* Windows client sets this to 64KiB */
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_QUERY_NETWORK_INTERFACE_INFO");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx, &net_iface,
			(ndr_pull_flags_fn_t)ndr_pull_fsctl_net_iface_info);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_fsctl_net_iface_info");

	ndr_print_debug((ndr_print_fn_t)ndr_print_fsctl_net_iface_info,
			"Network Interface Info", &net_iface);

	talloc_free(tmp_ctx);
	return true;
}

static NTSTATUS test_ioctl_sparse_fs_supported(struct torture_context *torture,
					       struct smb2_tree *tree,
					       TALLOC_CTX *mem_ctx,
					       struct smb2_handle *fh,
					       bool *sparse_support)
{
	NTSTATUS status;
	union smb_fsinfo info;

	ZERO_STRUCT(info);
	info.generic.level = RAW_QFS_ATTRIBUTE_INFORMATION;
	info.generic.handle = *fh;
	status = smb2_getinfo_fs(tree, tree, &info);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (info.attribute_info.out.fs_attr & FILE_SUPPORTS_SPARSE_FILES) {
		*sparse_support = true;
	} else {
		*sparse_support = false;
	}
	return NT_STATUS_OK;
}

static NTSTATUS test_ioctl_sparse_req(struct torture_context *torture,
				      TALLOC_CTX *mem_ctx,
				      struct smb2_tree *tree,
				      struct smb2_handle fh,
				      bool set)
{
	union smb_ioctl ioctl;
	NTSTATUS status;
	uint8_t set_sparse;

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_SPARSE;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;
	set_sparse = (set ? 0xFF : 0x0);
	ioctl.smb2.in.out.data = &set_sparse;
	ioctl.smb2.in.out.length = sizeof(set_sparse);

	status = smb2_ioctl(tree, mem_ctx, &ioctl.smb2);
	return status;
}

static NTSTATUS test_sparse_get(struct torture_context *torture,
				TALLOC_CTX *mem_ctx,
				struct smb2_tree *tree,
				struct smb2_handle fh,
				bool *_is_sparse)
{
	union smb_fileinfo io;
	NTSTATUS status;

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_BASIC_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, mem_ctx, &io);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}
	*_is_sparse = !!(io.basic_info.out.attrib & FILE_ATTRIBUTE_SPARSE);

	return status;
}

static bool test_ioctl_sparse_file_flag(struct torture_context *torture,
					struct smb2_tree *tree)
{
	struct smb2_handle fh;
	union smb_fileinfo io;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	ZERO_STRUCT(io);
	io.generic.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
	io.generic.in.file.handle = fh;
	status = smb2_getinfo_file(tree, tmp_ctx, &io);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FILE");

	torture_assert(torture,
		((io.all_info2.out.attrib & FILE_ATTRIBUTE_SPARSE) == 0),
		       "sparse attr before set");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "no sparse attr after set");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, false);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr after unset");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_file_attr(struct torture_context *torture,
					struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
			(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_SPARSE));
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr on open");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_dir_flag(struct torture_context *torture,
					struct smb2_tree *tree)
{
	struct smb2_handle dirh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;

	smb2_deltree(tree, DNAME);
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    DNAME, &dirh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_DIRECTORY);
	torture_assert(torture, ok, "setup sparse directory");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &dirh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, dirh);
		smb2_deltree(tree, DNAME);
		torture_skip(torture, "Sparse files not supported\n");
	}

	/* set sparse dir should fail, check for 2k12 & 2k8 response */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, dirh, true);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER,
				      "dir FSCTL_SET_SPARSE status");

	smb2_util_close(tree, dirh);
	smb2_deltree(tree, DNAME);
	talloc_free(tmp_ctx);
	return true;
}

/*
 * FSCTL_SET_SPARSE can be sent with (already tested) or without a SetSparse
 * buffer to indicate whether the flag should be set or cleared. When sent
 * without a buffer, it must be handled as if SetSparse=TRUE.
 */
static bool test_ioctl_sparse_set_nobuf(struct torture_context *torture,
					struct smb2_tree *tree)
{
	struct smb2_handle fh;
	union smb_ioctl ioctl;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr before set");

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_SPARSE;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;
	/* ioctl.smb2.in.out is zeroed, no SetSparse buffer */

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "no sparse attr after set");

	/* second non-SetSparse request shouldn't toggle sparse */
	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_SPARSE;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "no sparse attr after 2nd set");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, false);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr after unset");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_set_oversize(struct torture_context *torture,
					   struct smb2_tree *tree)
{
	struct smb2_handle fh;
	union smb_ioctl ioctl;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;
	uint8_t buf[100];

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr before set");

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_SPARSE;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	/*
	 * Attach a request buffer larger than FILE_SET_SPARSE_BUFFER
	 * Windows still successfully processes the request.
	 */
	ZERO_ARRAY(buf);
	buf[0] = 0xFF; /* attempt to set sparse */
	ioctl.smb2.in.out.data = buf;
	ioctl.smb2.in.out.length = ARRAY_SIZE(buf);

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "no sparse attr after set");

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_SPARSE;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	ZERO_ARRAY(buf); /* clear sparse */
	ioctl.smb2.in.out.data = buf;
	ioctl.smb2.in.out.length = ARRAY_SIZE(buf);

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr after clear");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static NTSTATUS test_ioctl_qar_req(struct torture_context *torture,
				   TALLOC_CTX *mem_ctx,
				   struct smb2_tree *tree,
				   struct smb2_handle fh,
				   int64_t req_off,
				   int64_t req_len,
				   struct file_alloced_range_buf **_rsp,
				   uint64_t *_rsp_count)
{
	union smb_ioctl ioctl;
	NTSTATUS status;
	enum ndr_err_code ndr_ret;
	struct file_alloced_range_buf far_buf;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;
	int i;
	TALLOC_CTX *tmp_ctx = talloc_new(mem_ctx);
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_QUERY_ALLOCATED_RANGES;
	ioctl.smb2.in.max_response_size = 1024;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	far_buf.file_off = req_off;
	far_buf.len = req_len;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &far_buf,
			(ndr_push_flags_fn_t)ndr_push_file_alloced_range_buf);
	if (ndr_ret != NDR_ERR_SUCCESS) {
		status = NT_STATUS_UNSUCCESSFUL;
		goto err_out;
	}

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	if (!NT_STATUS_IS_OK(status)) {
		goto err_out;
	}

	if (ioctl.smb2.out.out.length == 0) {
		goto done;
	}

	if ((ioctl.smb2.out.out.length % sizeof(far_buf)) != 0) {
		torture_comment(torture, "invalid qry_alloced rsp len: %zd:",
				ioctl.smb2.out.out.length);
		status = NT_STATUS_INVALID_VIEW_SIZE;
		goto err_out;
	}

	far_count = (ioctl.smb2.out.out.length / sizeof(far_buf));
	far_rsp = talloc_array(mem_ctx, struct file_alloced_range_buf,
			       far_count);
	if (far_rsp == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto err_out;
	}

	for (i = 0; i < far_count; i++) {
		ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
					       &far_rsp[i],
			(ndr_pull_flags_fn_t)ndr_pull_file_alloced_range_buf);
		if (ndr_ret != NDR_ERR_SUCCESS) {
			status = NT_STATUS_UNSUCCESSFUL;
			goto err_out;
		}
		/* move to next buffer */
		ioctl.smb2.out.out.data += sizeof(far_buf);
		ioctl.smb2.out.out.length -= sizeof(far_buf);
	}

done:
	*_rsp = far_rsp;
	*_rsp_count = far_count;
	status = NT_STATUS_OK;
err_out:
	talloc_free(tmp_ctx);
	return status;
}

static bool test_ioctl_sparse_qar(struct torture_context *torture,
				  struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	/* zero length file, shouldn't have any ranges */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr before set");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,	/* off */
				    0,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,	/* off */
				    1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "no sparse attr after set");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,	/* off */
				    1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");

	/* write into the (now) sparse file at 4k offset */
	ok = write_pattern(torture, tree, tmp_ctx, fh,
			   4096,	/* off */
			   1024,	/* len */
			   4096);	/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	/*
	 * Query range before write off. Whether it's allocated or not is FS
	 * dependent. NTFS deallocates chunks in 64K increments, but others
	 * (e.g. XFS, Btrfs, etc.) may deallocate 4K chunks.
	 */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,	/* off */
				    4096,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	if (far_count == 0) {
		torture_comment(torture, "FS deallocated 4K chunk\n");
	} else {
		/* expect fully allocated */
		torture_assert_u64_equal(torture, far_count, 1,
					 "unexpected response len");
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 0, "far offset");
		torture_assert_u64_equal(torture, far_rsp[0].len, 4096, "far len");
	}

	/*
	 * Query range before and past write, it should be allocated up to the
	 * end of the write.
	 */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,	/* off */
				    8192,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	/* FS dependent */
	if (far_rsp[0].file_off == 4096) {
		/* 4K chunk unallocated */
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 4096, "far offset");
		torture_assert_u64_equal(torture, far_rsp[0].len, 1024, "far len");
	} else {
		/* expect fully allocated */
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 0, "far offset");
		torture_assert_u64_equal(torture, far_rsp[0].len, 5120, "far len");
	}

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_qar_malformed(struct torture_context *torture,
					    struct smb2_tree *tree)
{
	struct smb2_handle fh;
	union smb_ioctl ioctl;
	struct file_alloced_range_buf far_buf;
	NTSTATUS status;
	enum ndr_err_code ndr_ret;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	size_t old_len;

	/* zero length file, shouldn't have any ranges */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	/* no allocated ranges, no space for range response, should pass */
	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_QUERY_ALLOCATED_RANGES;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	far_buf.file_off = 0;
	far_buf.len = 1024;
	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &far_buf,
			(ndr_push_flags_fn_t)ndr_push_file_alloced_range_buf);
	torture_assert_ndr_success(torture, ndr_ret, "push far ndr buf");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_QUERY_ALLOCATED_RANGES");

	/* write into the file at 4k offset */
	ok = write_pattern(torture, tree, tmp_ctx, fh,
			   0,		/* off */
			   1024,	/* len */
			   0);		/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	/* allocated range, no space for range response, should fail */
	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_BUFFER_TOO_SMALL, "qar no space");

	/* oversize (2x) file_alloced_range_buf in request, should pass */
	ioctl.smb2.in.max_response_size = 1024;
	old_len = ioctl.smb2.in.out.length;
	ok = data_blob_realloc(tmp_ctx, &ioctl.smb2.in.out,
			       (ioctl.smb2.in.out.length * 2));
	torture_assert(torture, ok, "2x data buffer");
	memcpy(ioctl.smb2.in.out.data + old_len, ioctl.smb2.in.out.data,
	       old_len);
	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "qar too big");

	/* no file_alloced_range_buf in request, should fail */
	data_blob_free(&ioctl.smb2.in.out);
	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER, "qar empty");

	return true;
}

/*
 * 2.3.57 FSCTL_SET_ZERO_DATA Request
 *
 * How an implementation zeros data within a file is implementation-dependent.
 * A file system MAY choose to deallocate regions of disk space that have been
 * zeroed.<50>
 * <50>
 * ... NTFS might deallocate disk space in the file if the file is stored on an
 * NTFS volume, and the file is sparse or compressed. It will free any allocated
 * space in chunks of 64 kilobytes that begin at an offset that is a multiple of
 * 64 kilobytes. Other bytes in the file (prior to the first freed 64-kilobyte
 * chunk and after the last freed 64-kilobyte chunk) will be zeroed but not
 * deallocated.
 */
static NTSTATUS test_ioctl_zdata_req(struct torture_context *torture,
				     TALLOC_CTX *mem_ctx,
				     struct smb2_tree *tree,
				     struct smb2_handle fh,
				     int64_t off,
				     int64_t beyond_final_zero)
{
	union smb_ioctl ioctl;
	NTSTATUS status;
	enum ndr_err_code ndr_ret;
	struct file_zero_data_info zdata_info;
	TALLOC_CTX *tmp_ctx = talloc_new(mem_ctx);
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_SET_ZERO_DATA;
	ioctl.smb2.in.max_response_size = 0;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	zdata_info.file_off = off;
	zdata_info.beyond_final_zero = beyond_final_zero;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &zdata_info,
			(ndr_push_flags_fn_t)ndr_push_file_zero_data_info);
	if (ndr_ret != NDR_ERR_SUCCESS) {
		status = NT_STATUS_UNSUCCESSFUL;
		goto err_out;
	}

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	if (!NT_STATUS_IS_OK(status)) {
		goto err_out;
	}

	status = NT_STATUS_OK;
err_out:
	talloc_free(tmp_ctx);
	return status;
}

static bool test_ioctl_sparse_punch(struct torture_context *torture,
				    struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 4096, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr before set");

	/* zero (hole-punch) the data, without sparse flag */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    4096,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");

	/* expect fully allocated */
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected far off");
	torture_assert_u64_equal(torture, far_rsp[0].len, 4096,
				 "unexpected far len");
	/* check that the data is now zeroed */
	ok = check_zero(torture, tree, tmp_ctx, fh, 0, 4096);
	torture_assert(torture, ok, "non-sparse zeroed range");

	/* set sparse */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	/* still fully allocated on NTFS, see note below for Samba */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    4096,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	/*
	 * FS specific: Samba uses PUNCH_HOLE to zero the range, and
	 * subsequently uses fallocate() to allocate the punched range if the
	 * file is marked non-sparse and "strict allocate" is enabled. In both
	 * cases, the zeroed range will not be detected by SEEK_DATA, so the
	 * range won't be present in QAR responses until the file is marked
	 * non-sparse again.
	 */
	if (far_count == 0) {
		torture_comment(torture, "non-sparse zeroed range disappeared "
				"after marking sparse\n");
	} else {
		/* NTFS: range remains fully allocated */
		torture_assert_u64_equal(torture, far_count, 1,
					 "unexpected response len");
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
					 "unexpected far off");
		torture_assert_u64_equal(torture, far_rsp[0].len, 4096,
					 "unexpected far len");
	}

	/* zero (hole-punch) the data, _with_ sparse flag */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	/* the range should no longer be alloced */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    4096,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");

	ok = check_zero(torture, tree, tmp_ctx, fh, 0, 4096);
	torture_assert(torture, ok, "sparse zeroed range");

	/* remove sparse flag, this should "unsparse" the zeroed range */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, false);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    4096,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	/* expect fully allocated */
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected far off");
	torture_assert_u64_equal(torture, far_rsp[0].len, 4096,
				 "unexpected far len");

	ok = check_zero(torture, tree, tmp_ctx, fh, 0, 4096);
	torture_assert(torture, ok, "sparse zeroed range");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

/*
 * Find the point at which a zeroed range in a sparse file is deallocated by the
 * underlying filesystem. NTFS on Windows Server 2012 deallocates chunks in 64k
 * increments. Also check whether zeroed neighbours are merged for deallocation.
 */
static bool test_ioctl_sparse_hole_dealloc(struct torture_context *torture,
					   struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint64_t file_size;
	uint64_t hlen;
	uint64_t dealloc_chunk_len = 0;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file 1");

	/* check for FS sparse file */
	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	/* set sparse */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	file_size = 1024 * 1024;

	ok = write_pattern(torture, tree, tmp_ctx, fh,
			   0,		/* off */
			   file_size,	/* len */
			   0);	/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	 /* check allocated ranges, should be fully allocated */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,			/* off */
				    file_size,		/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected far off");
	torture_assert_u64_equal(torture, far_rsp[0].len, file_size,
				 "unexpected far len");

	/* punch holes in sizes of 1k increments */
	for (hlen = 0; hlen <= file_size; hlen += 4096) {

		/* punch a hole from zero to the current increment */
		status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
					      0,	/* off */
					      hlen);	/* beyond_final_zero */
		torture_assert_ntstatus_ok(torture, status, "zero_data");

		/* ensure hole is zeroed, and pattern is consistent */
		ok = check_zero(torture, tree, tmp_ctx, fh, 0, hlen);
		torture_assert(torture, ok, "sparse zeroed range");

		ok = check_pattern(torture, tree, tmp_ctx, fh, hlen,
				   file_size - hlen, hlen);
		torture_assert(torture, ok, "allocated pattern range");

		 /* Check allocated ranges, hole might have been deallocated */
		status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
					    0,		/* off */
					    file_size,	/* len */
					    &far_rsp,
					    &far_count);
		torture_assert_ntstatus_ok(torture, status,
					   "FSCTL_QUERY_ALLOCATED_RANGES");
		if ((hlen == file_size) && (far_count == 0)) {
			/* hole covered entire file, deallocation occurred */
			dealloc_chunk_len = file_size;
			break;
		}

		torture_assert_u64_equal(torture, far_count, 1,
					 "unexpected response len");
		if (far_rsp[0].file_off != 0) {
			/*
			 * We now know the hole punch length needed to trigger a
			 * deallocation on this FS...
			 */
			dealloc_chunk_len = hlen;
			torture_comment(torture, "hole punch %ju@0 resulted in "
					"deallocation of %ju@0\n",
					(uintmax_t)hlen,
					(uintmax_t)far_rsp[0].file_off);
			torture_assert_u64_equal(torture,
						 file_size - far_rsp[0].len,
						 far_rsp[0].file_off,
						 "invalid alloced range");
			break;
		}
	}

	if (dealloc_chunk_len == 0) {
		torture_comment(torture, "strange, this FS never deallocates"
				"zeroed ranges in sparse files\n");
		return true;	/* FS specific, not a failure */
	}

	/*
	 * Check whether deallocation occurs when the (now known)
	 * deallocation chunk size is punched via two ZERO_DATA requests.
	 * I.e. Does the FS merge the two ranges and deallocate the chunk?
	 * NTFS on Windows Server 2012 does not.
	 */
	ok = write_pattern(torture, tree, tmp_ctx, fh,
			   0,		/* off */
			   file_size,	/* len */
			   0);	/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	/* divide dealloc chunk size by two, to use as punch length */
	hlen = dealloc_chunk_len >> 1;

	/*
	 *                     /half of dealloc chunk size           1M\
	 *                     |                                       |
	 * /offset 0           |                   /dealloc chunk size |
	 * |------------------ |-------------------|-------------------|
	 * | zeroed, 1st punch | zeroed, 2nd punch | existing pattern  |
	 */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      hlen);	/* beyond final zero */
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      hlen,	/* off */
				      dealloc_chunk_len); /* beyond final */
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	/* ensure holes are zeroed, and pattern is consistent */
	ok = check_zero(torture, tree, tmp_ctx, fh, 0, dealloc_chunk_len);
	torture_assert(torture, ok, "sparse zeroed range");

	ok = check_pattern(torture, tree, tmp_ctx, fh, dealloc_chunk_len,
			   file_size - dealloc_chunk_len, dealloc_chunk_len);
	torture_assert(torture, ok, "allocated pattern range");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,			/* off */
				    file_size,		/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");

	if ((far_count == 0) && (dealloc_chunk_len == file_size)) {
		torture_comment(torture, "holes merged for deallocation of "
				"full file\n");
		return true;
	}
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	if (far_rsp[0].file_off == dealloc_chunk_len) {
		torture_comment(torture, "holes merged for deallocation of "
				"%ju chunk\n", (uintmax_t)dealloc_chunk_len);
		torture_assert_u64_equal(torture,
					 file_size - far_rsp[0].len,
					 far_rsp[0].file_off,
					 "invalid alloced range");
	} else {
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
					 "unexpected deallocation");
		torture_comment(torture, "holes not merged for deallocation\n");
	}

	smb2_util_close(tree, fh);

	/*
	 * Check whether an unwritten range is allocated when a sparse file is
	 * written to at an offset past the dealloc chunk size:
	 *
	 *                     /dealloc chunk size
	 * /offset 0           |
	 * |------------------ |-------------------|
	 * |     unwritten     |      pattern      |
	 */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file 1");

	/* set sparse */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	ok = write_pattern(torture, tree, tmp_ctx, fh,
			   dealloc_chunk_len,	/* off */
			   1024,		/* len */
			   dealloc_chunk_len);	/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,				/* off */
				    dealloc_chunk_len + 1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	if (far_rsp[0].file_off == 0) {
		torture_assert_u64_equal(torture, far_rsp[0].len,
					 dealloc_chunk_len + 1024,
					 "unexpected far len");
		torture_comment(torture, "unwritten range fully allocated\n");
	} else {
		torture_assert_u64_equal(torture, far_rsp[0].file_off, dealloc_chunk_len,
					 "unexpected deallocation");
		torture_assert_u64_equal(torture, far_rsp[0].len, 1024,
					 "unexpected far len");
		torture_comment(torture, "unwritten range not allocated\n");
	}

	ok = check_zero(torture, tree, tmp_ctx, fh, 0, dealloc_chunk_len);
	torture_assert(torture, ok, "sparse zeroed range");

	ok = check_pattern(torture, tree, tmp_ctx, fh, dealloc_chunk_len,
			   1024, dealloc_chunk_len);
	torture_assert(torture, ok, "allocated pattern range");

	/* unsparse, should now be fully allocated */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, false);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,				/* off */
				    dealloc_chunk_len + 1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected deallocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len + 1024,
				 "unexpected far len");

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

/* check whether a file with compression and sparse attrs can be deallocated */
static bool test_ioctl_sparse_compressed(struct torture_context *torture,
					 struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint64_t file_size = 1024 * 1024;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file 1");

	/* check for FS sparse file and compression support */
	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_ioctl_compress_fs_supported(torture, tree, tmp_ctx, &fh,
						  &ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "FS compression not supported\n");
	}

	/* set compression and write some data */
	status = test_ioctl_compress_set(torture, tmp_ctx, tree, fh,
					 COMPRESSION_FORMAT_DEFAULT);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_COMPRESSION");

	ok = write_pattern(torture, tree, tmp_ctx, fh,
			   0,		/* off */
			   file_size,	/* len */
			   0);		/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	/* set sparse - now sparse and compressed */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	 /* check allocated ranges, should be fully alloced */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    file_size,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected far off");
	torture_assert_u64_equal(torture, far_rsp[0].len, file_size,
				 "unexpected far len");

	/* zero (hole-punch) all data, with sparse and compressed attrs */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,		/* off */
				      file_size);	/* beyond_final_zero */
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	 /*
	  * Windows Server 2012 still deallocates a zeroed range when a sparse
	  * file carries the compression attribute.
	  */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    file_size,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
				   "FSCTL_QUERY_ALLOCATED_RANGES req failed");
	if (far_count == 0) {
		torture_comment(torture, "sparse & compressed file "
				"deallocated after hole-punch\n");
	} else {
		torture_assert_u64_equal(torture, far_count, 1,
					 "unexpected response len");
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
					 "unexpected far off");
		torture_assert_u64_equal(torture, far_rsp[0].len, file_size,
					 "unexpected far len");
		torture_comment(torture, "sparse & compressed file fully "
				"allocated after hole-punch\n");
	}

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

/*
 * Create a sparse file, then attempt to copy unallocated and allocated ranges
 * into a target file using FSCTL_SRV_COPYCHUNK.
 */
static bool test_ioctl_sparse_copy_chunk(struct torture_context *torture,
					 struct smb2_tree *tree)
{
	struct smb2_handle src_h;
	struct smb2_handle dest_h;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint64_t dealloc_chunk_len = 64 * 1024;	/* Windows 2012 */
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;
	union smb_ioctl ioctl;
	struct srv_copychunk_copy cc_copy;
	struct srv_copychunk_rsp cc_rsp;
	enum ndr_err_code ndr_ret;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &src_h, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	/* check for FS sparse file support */
	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &src_h,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	smb2_util_close(tree, src_h);
	if (!ok) {
		torture_skip(torture, "Sparse files not supported\n");
	}

	ok = test_setup_copy_chunk(torture, tree, tmp_ctx,
				   1, /* chunks */
				   &src_h, 0, /* src file */
				   SEC_RIGHTS_FILE_ALL,
				   &dest_h, 0,	/* dest file */
				   SEC_RIGHTS_FILE_ALL,
				   &cc_copy,
				   &ioctl);
	torture_assert(torture, ok, "setup copy chunk error");

	/* set sparse */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, src_h, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	/* start after dealloc_chunk_len, to create an unwritten sparse range */
	ok = write_pattern(torture, tree, tmp_ctx, src_h,
			   dealloc_chunk_len,	/* off */
			   1024,	/* len */
			   dealloc_chunk_len);	/* pattern offset */
	torture_assert(torture, ok, "write pattern");

	 /* Skip test if 64k chunk is allocated - FS specific */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, src_h,
				    0,				/* off */
				    dealloc_chunk_len + 1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	if (far_rsp[0].file_off == 0) {
		torture_skip(torture, "unwritten range fully allocated\n");
	}

	torture_assert_u64_equal(torture, far_rsp[0].file_off, dealloc_chunk_len,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len, 1024,
				 "unexpected far len");

	/* copy-chunk unallocated + written ranges into non-sparse dest */

	cc_copy.chunks[0].source_off = 0;
	cc_copy.chunks[0].target_off = 0;
	cc_copy.chunks[0].length = dealloc_chunk_len + 1024;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &cc_copy,
			(ndr_push_flags_fn_t)ndr_push_srv_copychunk_copy);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_srv_copychunk_copy");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  dealloc_chunk_len + 1024); /* bytes written */
	torture_assert(torture, ok, "bad copy chunk response data");

	ok = check_zero(torture, tree, tmp_ctx, dest_h, 0, dealloc_chunk_len);
	torture_assert(torture, ok, "sparse zeroed range");

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, dealloc_chunk_len,
			   1024, dealloc_chunk_len);
	torture_assert(torture, ok, "copychunked range");

	/* copied range should be allocated in non-sparse dest */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, dest_h,
				    0,				/* off */
				    dealloc_chunk_len + 1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len + 1024,
				 "unexpected far len");

	/* set dest as sparse */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, dest_h, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	/* zero (hole-punch) all data */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, dest_h,
				      0,		/* off */
				      dealloc_chunk_len + 1024);
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	/* zeroed range might be deallocated */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, dest_h,
				    0,				/* off */
				    dealloc_chunk_len + 1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	if (far_count == 0) {
		/* FS specific (e.g. NTFS) */
		torture_comment(torture, "FS deallocates file on full-range "
				"punch\n");
	} else {
		/* FS specific (e.g. EXT4) */
		torture_comment(torture, "FS doesn't deallocate file on "
				"full-range punch\n");
	}
	ok = check_zero(torture, tree, tmp_ctx, dest_h, 0,
			dealloc_chunk_len + 1024);
	torture_assert(torture, ok, "punched zeroed range");

	/* copy-chunk again, this time with sparse dest */
	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SRV_COPYCHUNK");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &cc_rsp,
			(ndr_pull_flags_fn_t)ndr_pull_srv_copychunk_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_srv_copychunk_rsp");

	ok = check_copy_chunk_rsp(torture, &cc_rsp,
				  1,	/* chunks written */
				  0,	/* chunk bytes unsuccessfully written */
				  dealloc_chunk_len + 1024); /* bytes written */
	torture_assert(torture, ok, "bad copy chunk response data");

	ok = check_zero(torture, tree, tmp_ctx, dest_h, 0, dealloc_chunk_len);
	torture_assert(torture, ok, "sparse zeroed range");

	ok = check_pattern(torture, tree, tmp_ctx, dest_h, dealloc_chunk_len,
			   1024, dealloc_chunk_len);
	torture_assert(torture, ok, "copychunked range");

	/* copied range may be allocated in sparse dest */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, dest_h,
				    0,				/* off */
				    dealloc_chunk_len + 1024,	/* len */
				    &far_rsp,
				    &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	/*
	 * FS specific: sparse region may be unallocated in dest if copy-chunk
	 *		is handled in a sparse preserving way - E.g. vfs_btrfs
	 *		with BTRFS_IOC_CLONE_RANGE.
	 */
	if (far_rsp[0].file_off == dealloc_chunk_len) {
		torture_comment(torture, "copy-chunk sparse range preserved\n");
		torture_assert_u64_equal(torture, far_rsp[0].len, 1024,
					 "unexpected far len");
	} else {
		torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
					 "unexpected allocation");
		torture_assert_u64_equal(torture, far_rsp[0].len,
					 dealloc_chunk_len + 1024,
					 "unexpected far len");
	}

	smb2_util_close(tree, src_h);
	smb2_util_close(tree, dest_h);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_punch_invalid(struct torture_context *torture,
					    struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;
	int i;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 4096, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse attr before set");

	/* loop twice, without and with sparse attrib */
	for (i = 0; i <= 1;  i++) {
		union smb_fileinfo io;
		struct file_alloced_range_buf *far_rsp = NULL;
		uint64_t far_count = 0;

		/* get size before & after. zero data should never change it */
		ZERO_STRUCT(io);
		io.generic.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
		io.generic.in.file.handle = fh;
		status = smb2_getinfo_file(tree, tmp_ctx, &io);
		torture_assert_ntstatus_ok(torture, status, "getinfo");
		torture_assert_int_equal(torture, (int)io.all_info2.out.size,
					 4096, "size after IO");

		/* valid 8 byte zero data, but after EOF */
		status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
					      4096,	/* off */
					      4104);	/* beyond_final_zero */
		torture_assert_ntstatus_ok(torture, status, "zero_data");

		/* valid 8 byte zero data, but after EOF */
		status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
					      8192,	/* off */
					      8200);	/* beyond_final_zero */
		torture_assert_ntstatus_ok(torture, status, "zero_data");

		ZERO_STRUCT(io);
		io.generic.level = RAW_FILEINFO_SMB2_ALL_INFORMATION;
		io.generic.in.file.handle = fh;
		status = smb2_getinfo_file(tree, tmp_ctx, &io);
		torture_assert_ntstatus_ok(torture, status, "getinfo");
		torture_assert_int_equal(torture, (int)io.all_info2.out.size,
					 4096, "size after IO");

		/* valid 0 byte zero data, without sparse flag */
		status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
					      4095,	/* off */
					      4095);	/* beyond_final_zero */
		torture_assert_ntstatus_ok(torture, status, "zero_data");

		/* INVALID off is past beyond_final_zero */
		status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
					      4096,	/* off */
					      4095);	/* beyond_final_zero */
		torture_assert_ntstatus_equal(torture, status,
					      NT_STATUS_INVALID_PARAMETER,
					      "invalid zero_data");

		/* zero length QAR - valid */
		status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
					    0,			/* off */
					    0,			/* len */
					    &far_rsp, &far_count);
		torture_assert_ntstatus_ok(torture, status,
				"FSCTL_QUERY_ALLOCATED_RANGES req failed");
		torture_assert_u64_equal(torture, far_count, 0,
					 "unexpected response len");

		/* QAR after EOF - valid */
		status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
					    4096,		/* off */
					    1024,		/* len */
					    &far_rsp, &far_count);
		torture_assert_ntstatus_ok(torture, status,
				"FSCTL_QUERY_ALLOCATED_RANGES req failed");
		torture_assert_u64_equal(torture, far_count, 0,
					 "unexpected response len");

		/* set sparse */
		status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh,
					       true);
		torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");
	}

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_perms(struct torture_context *torture,
				    struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	smb2_util_close(tree, fh);
	if (!ok) {
		torture_skip(torture, "Sparse files not supported\n");
	}

	/* set sparse without WRITE_ATTR permission should succeed */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_WRITE & ~(SEC_FILE_WRITE_ATTRIBUTE
							| SEC_STD_WRITE_DAC
							| SEC_FILE_WRITE_EA)),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");
	smb2_util_close(tree, fh);

	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_RIGHTS_FILE_ALL,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "sparse after set");
	smb2_util_close(tree, fh);

	/* attempt get sparse without READ_DATA permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
			(SEC_RIGHTS_FILE_READ & ~SEC_FILE_READ_DATA),
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse set");
	smb2_util_close(tree, fh);

	/* attempt to set sparse with only WRITE_ATTR permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
				    SEC_FILE_WRITE_ATTRIBUTE,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");
	smb2_util_close(tree, fh);

	/* attempt to set sparse with only WRITE_DATA permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
				    SEC_FILE_WRITE_DATA,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");
	smb2_util_close(tree, fh);

	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_RIGHTS_FILE_ALL,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "sparse after set");
	smb2_util_close(tree, fh);

	/* attempt to set sparse with only APPEND_DATA permission */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
				    SEC_FILE_APPEND_DATA,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");
	smb2_util_close(tree, fh);

	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_RIGHTS_FILE_ALL,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "sparse after set");
	smb2_util_close(tree, fh);

	/* attempt to set sparse with only WRITE_EA permission - should fail */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 0,
				    SEC_FILE_WRITE_EA,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "FSCTL_SET_SPARSE permission");
	smb2_util_close(tree, fh);

	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_RIGHTS_FILE_ALL,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, !is_sparse, "sparse after set");
	smb2_util_close(tree, fh);

	/* attempt QAR with only READ_ATTR permission - should fail */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_READ_ATTRIBUTE,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    4096,		/* off */
				    1024,		/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
			"FSCTL_QUERY_ALLOCATED_RANGES req passed");
	smb2_util_close(tree, fh);

	/* attempt QAR with only READ_DATA permission */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_READ_DATA,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    1024,		/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");
	smb2_util_close(tree, fh);

	/* attempt QAR with only READ_EA permission - should fail */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_READ_EA,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    4096,		/* off */
				    1024,		/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
			"FSCTL_QUERY_ALLOCATED_RANGES req passed");
	smb2_util_close(tree, fh);

	/* setup file for ZERO_DATA permissions tests */
	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 8192,
				    SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");
	smb2_util_close(tree, fh);

	/* attempt ZERO_DATA with only WRITE_ATTR permission - should fail */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_WRITE_ATTRIBUTE,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "zero_data permission");
	smb2_util_close(tree, fh);

	/* attempt ZERO_DATA with only WRITE_DATA permission */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_WRITE_DATA,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_ok(torture, status, "zero_data");
	smb2_util_close(tree, fh);

	/* attempt ZERO_DATA with only APPEND_DATA permission - should fail */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_APPEND_DATA,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "zero_data permission");
	smb2_util_close(tree, fh);

	/* attempt ZERO_DATA with only WRITE_EA permission - should fail */
	ok = test_setup_open(torture, tree, tmp_ctx,
			     FNAME, &fh, SEC_FILE_WRITE_EA,
			     FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_ACCESS_DENIED,
				      "zero_data permission");
	smb2_util_close(tree, fh);

	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_lck(struct torture_context *torture,
				  struct smb2_tree *tree)
{
	struct smb2_handle fh;
	struct smb2_handle fh2;
	NTSTATUS status;
	uint64_t dealloc_chunk_len = 64 * 1024;	/* Windows 2012 */
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	bool is_sparse;
	struct smb2_lock lck;
	struct smb2_lock_element el[1];
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx, FNAME, &fh,
				    dealloc_chunk_len, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		torture_skip(torture, "Sparse files not supported\n");
		smb2_util_close(tree, fh);
	}

	/* open and lock via separate fh2 */
	status = torture_smb2_testfile(tree, FNAME, &fh2);
	torture_assert_ntstatus_ok(torture, status, "2nd src open");

	lck.in.lock_count	= 0x0001;
	lck.in.lock_sequence	= 0x00000000;
	lck.in.file.handle	= fh2;
	lck.in.locks		= el;
	el[0].offset		= 0;
	el[0].length		= dealloc_chunk_len;
	el[0].reserved		= 0;
	el[0].flags		= SMB2_LOCK_FLAG_EXCLUSIVE;

	status = smb2_lock(tree, &lck);
	torture_assert_ntstatus_ok(torture, status, "lock");

	/* set sparse while locked */
	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	status = test_sparse_get(torture, tmp_ctx, tree, fh, &is_sparse);
	torture_assert_ntstatus_ok(torture, status, "test_sparse_get");
	torture_assert(torture, is_sparse, "sparse attr after set");

	/* zero data over locked range should fail */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      0,	/* off */
				      4096);	/* beyond_final_zero */
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_FILE_LOCK_CONFLICT,
				      "zero_data locked");

	/* QAR over locked range should pass */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,		/* off */
				    4096,	/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES locked");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 4096,
				 "unexpected far len");

	/* zero data over range past EOF should pass */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      dealloc_chunk_len,	/* off */
				      dealloc_chunk_len + 4096);
	torture_assert_ntstatus_ok(torture, status,
				   "zero_data past EOF locked");

	/* QAR over range past EOF should pass */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    dealloc_chunk_len,		/* off */
				    4096,			/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES past EOF locked");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");

	lck.in.lock_count	= 0x0001;
	lck.in.lock_sequence	= 0x00000001;
	lck.in.file.handle	= fh2;
	lck.in.locks		= el;
	el[0].offset		= 0;
	el[0].length		= dealloc_chunk_len;
	el[0].reserved		= 0;
	el[0].flags		= SMB2_LOCK_FLAG_UNLOCK;
	status = smb2_lock(tree, &lck);
	torture_assert_ntstatus_ok(torture, status, "unlock");

	smb2_util_close(tree, fh2);
	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

/* alleviate QAR off-by-one bug paranoia - help me ob1 */
static bool test_ioctl_sparse_qar_ob1(struct torture_context *torture,
				      struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint64_t dealloc_chunk_len = 64 * 1024;	/* Windows 2012 */
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, dealloc_chunk_len * 2,
				    SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		torture_skip(torture, "Sparse files not supported\n");
		smb2_util_close(tree, fh);
	}

	/* non-sparse QAR with range one before EOF */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,				/* off */
				    dealloc_chunk_len * 2 - 1,	/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len * 2 - 1,
				 "unexpected far len");

	/* non-sparse QAR with range one after EOF */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,				/* off */
				    dealloc_chunk_len * 2 + 1,	/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len * 2,
				 "unexpected far len");

	/* non-sparse QAR with range one after EOF from off=1 */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    1,				/* off */
				    dealloc_chunk_len * 2,	/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 1,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len * 2 - 1,
				 "unexpected far len");

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	/* punch out second chunk */
	status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
				      dealloc_chunk_len,	/* off */
				      dealloc_chunk_len * 2);
	torture_assert_ntstatus_ok(torture, status, "zero_data");

	/* sparse QAR with range one before hole */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,				/* off */
				    dealloc_chunk_len - 1,	/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len - 1,
				 "unexpected far len");

	/* sparse QAR with range one after hole */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,				/* off */
				    dealloc_chunk_len + 1,	/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 0,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len,
				 "unexpected far len");

	/* sparse QAR with range one after hole from off=1 */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    1,				/* off */
				    dealloc_chunk_len,		/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off, 1,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 dealloc_chunk_len - 1,
				 "unexpected far len");

	/* sparse QAR with range one before EOF from off=chunk_len-1 */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    dealloc_chunk_len - 1,	/* off */
				    dealloc_chunk_len,		/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 1,
				 "unexpected response len");
	torture_assert_u64_equal(torture, far_rsp[0].file_off,
				 dealloc_chunk_len - 1,
				 "unexpected allocation");
	torture_assert_u64_equal(torture, far_rsp[0].len,
				 1, "unexpected far len");

	/* sparse QAR with range one after EOF from off=chunk_len+1 */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    dealloc_chunk_len + 1,	/* off */
				    dealloc_chunk_len,		/* len */
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	torture_assert_u64_equal(torture, far_count, 0,
				 "unexpected response len");
	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

/* test QAR with multi-range responses */
static bool test_ioctl_sparse_qar_multi(struct torture_context *torture,
					struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;
	uint64_t dealloc_chunk_len = 64 * 1024;	/* Windows 2012 */
	uint64_t this_off;
	int i;
	struct file_alloced_range_buf *far_rsp = NULL;
	uint64_t far_count = 0;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, dealloc_chunk_len * 2,
				    SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		torture_skip(torture, "Sparse files not supported\n");
		smb2_util_close(tree, fh);
	}

	status = test_ioctl_sparse_req(torture, tmp_ctx, tree, fh, true);
	torture_assert_ntstatus_ok(torture, status, "FSCTL_SET_SPARSE");

	/* each loop, write out two chunks and punch the first out */
	for (i = 0; i < 10; i++) {
		this_off = i * dealloc_chunk_len * 2;

		ok = write_pattern(torture, tree, tmp_ctx, fh,
				   this_off,			/* off */
				   dealloc_chunk_len * 2,	/* len */
				   this_off);		/* pattern offset */
		torture_assert(torture, ok, "write pattern");

		status = test_ioctl_zdata_req(torture, tmp_ctx, tree, fh,
					      this_off,	/* off */
					      this_off + dealloc_chunk_len);
		torture_assert_ntstatus_ok(torture, status, "zero_data");
	}

	/* should now have one separate region for each iteration */
	status = test_ioctl_qar_req(torture, tmp_ctx, tree, fh,
				    0,
				    10 * dealloc_chunk_len * 2,
				    &far_rsp, &far_count);
	torture_assert_ntstatus_ok(torture, status,
			"FSCTL_QUERY_ALLOCATED_RANGES req failed");
	if (far_count == 1) {
		torture_comment(torture, "this FS doesn't deallocate 64K"
				"zeroed ranges in sparse files\n");
		return true;	/* FS specific, not a failure */
	}
	torture_assert_u64_equal(torture, far_count, 10,
				 "unexpected response len");
	for (i = 0; i < 10; i++) {
		this_off = i * dealloc_chunk_len * 2;

		torture_assert_u64_equal(torture, far_rsp[i].file_off,
					 this_off + dealloc_chunk_len,
					 "unexpected allocation");
		torture_assert_u64_equal(torture, far_rsp[i].len,
					 dealloc_chunk_len,
					 "unexpected far len");
	}

	smb2_util_close(tree, fh);
	talloc_free(tmp_ctx);
	return true;
}

static bool test_ioctl_sparse_qar_overflow(struct torture_context *torture,
					   struct smb2_tree *tree)
{
	struct smb2_handle fh;
	union smb_ioctl ioctl;
	struct file_alloced_range_buf far_buf;
	NTSTATUS status;
	enum ndr_err_code ndr_ret;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	bool ok;

	ok = test_setup_create_fill(torture, tree, tmp_ctx,
				    FNAME, &fh, 1024, SEC_RIGHTS_FILE_ALL,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "setup file");

	status = test_ioctl_sparse_fs_supported(torture, tree, tmp_ctx, &fh,
						&ok);
	torture_assert_ntstatus_ok(torture, status, "SMB2_GETINFO_FS");
	if (!ok) {
		smb2_util_close(tree, fh);
		torture_skip(torture, "Sparse files not supported\n");
	}

	/* no allocated ranges, no space for range response, should pass */
	ZERO_STRUCT(ioctl);
	ioctl.smb2.level = RAW_IOCTL_SMB2;
	ioctl.smb2.in.file.handle = fh;
	ioctl.smb2.in.function = FSCTL_QUERY_ALLOCATED_RANGES;
	ioctl.smb2.in.max_response_size = 1024;
	ioctl.smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	/* off + length wraps around to 511 */
	far_buf.file_off = 512;
	far_buf.len = 0xffffffffffffffffLL;
	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx,
				       &far_buf,
			(ndr_push_flags_fn_t)ndr_push_file_alloced_range_buf);
	torture_assert_ndr_success(torture, ndr_ret, "push far ndr buf");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_equal(torture, status,
				      NT_STATUS_INVALID_PARAMETER,
				      "FSCTL_QUERY_ALLOCATED_RANGES overflow");

	return true;
}

static NTSTATUS test_ioctl_trim_supported(struct torture_context *torture,
					  struct smb2_tree *tree,
					  TALLOC_CTX *mem_ctx,
					  struct smb2_handle *fh,
					  bool *trim_support)
{
	NTSTATUS status;
	union smb_fsinfo info;

	ZERO_STRUCT(info);
	info.generic.level = RAW_QFS_SECTOR_SIZE_INFORMATION;
	info.generic.handle = *fh;
	status = smb2_getinfo_fs(tree, tree, &info);
	if (NT_STATUS_EQUAL(status, NT_STATUS_INVALID_INFO_CLASS)) {
		/*
		 * Windows < Server 2012, 8 etc. don't support this info level
		 * or the trim ioctl. Ignore the error and let the caller skip.
		 */
		*trim_support = false;
		return NT_STATUS_OK;
	} else if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	torture_comment(torture, "sector size info: lb/s=%u, pb/sA=%u, "
			"pb/sP=%u, fse/sA=%u, flags=0x%x, bosa=%u, bopa=%u\n",
	    (unsigned)info.sector_size_info.out.logical_bytes_per_sector,
	    (unsigned)info.sector_size_info.out.phys_bytes_per_sector_atomic,
	    (unsigned)info.sector_size_info.out.phys_bytes_per_sector_perf,
  (unsigned)info.sector_size_info.out.fs_effective_phys_bytes_per_sector_atomic,
	    (unsigned)info.sector_size_info.out.flags,
	    (unsigned)info.sector_size_info.out.byte_off_sector_align,
	    (unsigned)info.sector_size_info.out.byte_off_partition_align);

	if (info.sector_size_info.out.flags & QFS_SSINFO_FLAGS_TRIM_ENABLED) {
		*trim_support = true;
	} else {
		*trim_support = false;
	}
	return NT_STATUS_OK;
}

static bool test_setup_trim(struct torture_context *torture,
			    struct smb2_tree *tree,
			    TALLOC_CTX *mem_ctx,
			    uint32_t num_ranges,
			    struct smb2_handle *fh,
			    uint64_t file_size,
			    uint32_t desired_access,
			    struct fsctl_file_level_trim_req *trim_req,
			    union smb_ioctl *ioctl)
{
	bool ok;

	ok = test_setup_create_fill(torture, tree, mem_ctx, FNAME,
				    fh, file_size, desired_access,
				    FILE_ATTRIBUTE_NORMAL);
	torture_assert(torture, ok, "src file create fill");

	ZERO_STRUCTPN(ioctl);
	ioctl->smb2.level = RAW_IOCTL_SMB2;
	ioctl->smb2.in.file.handle = *fh;
	ioctl->smb2.in.function = FSCTL_FILE_LEVEL_TRIM;
	ioctl->smb2.in.max_response_size
				= sizeof(struct fsctl_file_level_trim_rsp);
	ioctl->smb2.in.flags = SMB2_IOCTL_FLAG_IS_FSCTL;

	ZERO_STRUCTPN(trim_req);
	/* leave key as zero for now. TODO test locking with differing keys */
	trim_req->num_ranges = num_ranges;
	trim_req->ranges = talloc_zero_array(mem_ctx,
					     struct file_level_trim_range,
					     num_ranges);
	torture_assert(torture, (trim_req->ranges != NULL), "no memory for ranges");

	return true;
}

static bool test_ioctl_trim_simple(struct torture_context *torture,
				   struct smb2_tree *tree)
{
	struct smb2_handle fh;
	NTSTATUS status;
	union smb_ioctl ioctl;
	bool trim_supported;
	TALLOC_CTX *tmp_ctx = talloc_new(tree);
	struct fsctl_file_level_trim_req trim_req;
	struct fsctl_file_level_trim_rsp trim_rsp;
	uint64_t trim_chunk_len = 64 * 1024;	/* trim 64K chunks */
	enum ndr_err_code ndr_ret;
	bool ok;

	ok = test_setup_trim(torture, tree, tmp_ctx,
			     1, /* 1 range */
			     &fh, 2 * trim_chunk_len, /* fill 128K file */
			     SEC_RIGHTS_FILE_ALL,
			     &trim_req,
			     &ioctl);
	if (!ok) {
		torture_fail(torture, "setup trim error");
	}

	status = test_ioctl_trim_supported(torture, tree, tmp_ctx, &fh,
					   &trim_supported);
	torture_assert_ntstatus_ok(torture, status, "fsinfo");
	if (!trim_supported) {
		smb2_util_close(tree, fh);
		talloc_free(tmp_ctx);
		torture_skip(torture, "trim not supported\n");
	}

	/* trim first chunk, leave second */
	trim_req.ranges[0].off = 0;
	trim_req.ranges[0].len = trim_chunk_len;

	ndr_ret = ndr_push_struct_blob(&ioctl.smb2.in.out, tmp_ctx, &trim_req,
		       (ndr_push_flags_fn_t)ndr_push_fsctl_file_level_trim_req);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_push_fsctl_file_level_trim_req");

	status = smb2_ioctl(tree, tmp_ctx, &ioctl.smb2);
	torture_assert_ntstatus_ok(torture, status, "FILE_LEVEL_TRIM_RANGE");

	ndr_ret = ndr_pull_struct_blob(&ioctl.smb2.out.out, tmp_ctx,
				       &trim_rsp,
		       (ndr_pull_flags_fn_t)ndr_pull_fsctl_file_level_trim_rsp);
	torture_assert_ndr_success(torture, ndr_ret,
				   "ndr_pull_fsctl_file_level_trim_rsp");

	torture_assert_int_equal(torture, trim_rsp.num_ranges_processed, 1, "");

	/* second half of the file should remain consitent */
	ok = check_pattern(torture, tree, tmp_ctx, fh, trim_chunk_len,
			   trim_chunk_len, trim_chunk_len);
	torture_assert(torture, ok, "non-trimmed range inconsistent");

	return true;
}

/*
 * basic testing of SMB2 ioctls
 */
struct torture_suite *torture_smb2_ioctl_init(void)
{
	struct torture_suite *suite = torture_suite_create(talloc_autofree_context(), "ioctl");

	torture_suite_add_1smb2_test(suite, "shadow_copy",
				     test_ioctl_get_shadow_copy);
	torture_suite_add_1smb2_test(suite, "req_resume_key",
				     test_ioctl_req_resume_key);
	torture_suite_add_1smb2_test(suite, "copy_chunk_simple",
				     test_ioctl_copy_chunk_simple);
	torture_suite_add_1smb2_test(suite, "copy_chunk_multi",
				     test_ioctl_copy_chunk_multi);
	torture_suite_add_1smb2_test(suite, "copy_chunk_tiny",
				     test_ioctl_copy_chunk_tiny);
	torture_suite_add_1smb2_test(suite, "copy_chunk_overwrite",
				     test_ioctl_copy_chunk_over);
	torture_suite_add_1smb2_test(suite, "copy_chunk_append",
				     test_ioctl_copy_chunk_append);
	torture_suite_add_1smb2_test(suite, "copy_chunk_limits",
				     test_ioctl_copy_chunk_limits);
	torture_suite_add_1smb2_test(suite, "copy_chunk_src_lock",
				     test_ioctl_copy_chunk_src_lck);
	torture_suite_add_1smb2_test(suite, "copy_chunk_dest_lock",
				     test_ioctl_copy_chunk_dest_lck);
	torture_suite_add_1smb2_test(suite, "copy_chunk_bad_key",
				     test_ioctl_copy_chunk_bad_key);
	torture_suite_add_1smb2_test(suite, "copy_chunk_src_is_dest",
				     test_ioctl_copy_chunk_src_is_dest);
	torture_suite_add_1smb2_test(suite, "copy_chunk_src_is_dest_overlap",
				     test_ioctl_copy_chunk_src_is_dest_overlap);
	torture_suite_add_1smb2_test(suite, "copy_chunk_bad_access",
				     test_ioctl_copy_chunk_bad_access);
	torture_suite_add_1smb2_test(suite, "copy_chunk_write_access",
				     test_ioctl_copy_chunk_write_access);
	torture_suite_add_1smb2_test(suite, "copy_chunk_src_exceed",
				     test_ioctl_copy_chunk_src_exceed);
	torture_suite_add_1smb2_test(suite, "copy_chunk_src_exceed_multi",
				     test_ioctl_copy_chunk_src_exceed_multi);
	torture_suite_add_1smb2_test(suite, "copy_chunk_sparse_dest",
				     test_ioctl_copy_chunk_sparse_dest);
	torture_suite_add_1smb2_test(suite, "copy_chunk_max_output_sz",
				     test_ioctl_copy_chunk_max_output_sz);
	torture_suite_add_1smb2_test(suite, "copy_chunk_zero_length",
				     test_ioctl_copy_chunk_zero_length);
	torture_suite_add_1smb2_test(suite, "compress_file_flag",
				     test_ioctl_compress_file_flag);
	torture_suite_add_1smb2_test(suite, "compress_dir_inherit",
				     test_ioctl_compress_dir_inherit);
	torture_suite_add_1smb2_test(suite, "compress_invalid_format",
				     test_ioctl_compress_invalid_format);
	torture_suite_add_1smb2_test(suite, "compress_invalid_buf",
				     test_ioctl_compress_invalid_buf);
	torture_suite_add_1smb2_test(suite, "compress_query_file_attr",
				     test_ioctl_compress_query_file_attr);
	torture_suite_add_1smb2_test(suite, "compress_create_with_attr",
				     test_ioctl_compress_create_with_attr);
	torture_suite_add_1smb2_test(suite, "compress_inherit_disable",
				     test_ioctl_compress_inherit_disable);
	torture_suite_add_1smb2_test(suite, "compress_set_file_attr",
				     test_ioctl_compress_set_file_attr);
	torture_suite_add_1smb2_test(suite, "compress_perms",
				     test_ioctl_compress_perms);
	torture_suite_add_1smb2_test(suite, "network_interface_info",
				     test_ioctl_network_interface_info);
	torture_suite_add_1smb2_test(suite, "sparse_file_flag",
				     test_ioctl_sparse_file_flag);
	torture_suite_add_1smb2_test(suite, "sparse_file_attr",
				     test_ioctl_sparse_file_attr);
	torture_suite_add_1smb2_test(suite, "sparse_dir_flag",
				     test_ioctl_sparse_dir_flag);
	torture_suite_add_1smb2_test(suite, "sparse_set_nobuf",
				     test_ioctl_sparse_set_nobuf);
	torture_suite_add_1smb2_test(suite, "sparse_set_oversize",
				     test_ioctl_sparse_set_oversize);
	torture_suite_add_1smb2_test(suite, "sparse_qar",
				     test_ioctl_sparse_qar);
	torture_suite_add_1smb2_test(suite, "sparse_qar_malformed",
				     test_ioctl_sparse_qar_malformed);
	torture_suite_add_1smb2_test(suite, "sparse_punch",
				     test_ioctl_sparse_punch);
	torture_suite_add_1smb2_test(suite, "sparse_hole_dealloc",
				     test_ioctl_sparse_hole_dealloc);
	torture_suite_add_1smb2_test(suite, "sparse_compressed",
				     test_ioctl_sparse_compressed);
	torture_suite_add_1smb2_test(suite, "sparse_copy_chunk",
				     test_ioctl_sparse_copy_chunk);
	torture_suite_add_1smb2_test(suite, "sparse_punch_invalid",
				     test_ioctl_sparse_punch_invalid);
	torture_suite_add_1smb2_test(suite, "sparse_perms",
				     test_ioctl_sparse_perms);
	torture_suite_add_1smb2_test(suite, "sparse_lock",
				     test_ioctl_sparse_lck);
	torture_suite_add_1smb2_test(suite, "sparse_qar_ob1",
				     test_ioctl_sparse_qar_ob1);
	torture_suite_add_1smb2_test(suite, "sparse_qar_multi",
				     test_ioctl_sparse_qar_multi);
	torture_suite_add_1smb2_test(suite, "sparse_qar_overflow",
				     test_ioctl_sparse_qar_overflow);
	torture_suite_add_1smb2_test(suite, "trim_simple",
				     test_ioctl_trim_simple);

	suite->description = talloc_strdup(suite, "SMB2-IOCTL tests");

	return suite;
}

