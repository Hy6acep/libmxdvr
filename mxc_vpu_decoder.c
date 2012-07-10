#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
//#include <fcntl.h>
//#include <sys/stat.h>
#include <errno.h>
//#include <linux/videodev2.h>
#include "mxc_defs.h"
#include "mxc_vpu.h"
#include "mxc_display.h"

#define timer_start; \
{\
	struct timeval ts, te;\
	int elapsed;\
	gettimeofday(&ts, NULL);

#define timer_stop; \
	gettimeofday(&te, NULL);\
	elapsed = (te.tv_sec - ts.tv_sec) * 1000000 + (te.tv_usec - ts.tv_usec);\
	fprintf(stderr, "encoding %d ms\n", elapsed/1000);\
}

static int freadn(int fd, void *vptr, size_t n)
{
	int nleft = 0;
	int nread = 0;
	char *ptr;

	ptr = vptr;
	nleft = n;
	while(nleft > 0)
	{
		if((nread = read(fd, ptr, nleft)) <= 0)
		{
			if(nread == 0)
				return (n - nleft);

			perror("read");
			return (-1);
		}

		nleft -= nread;
		ptr += nread;
	}

	return (nread);
}

static int vpu_read(int fd, char *buf, int n)
{
	return freadn(fd, buf, n);
}


static int fill_bsbuffer(DecodingInstance dec, int defaultsize, int *eos, int *fill_end_bs)
{
	RetCode ret = 0;
	DecHandle handle = dec->handle;
	PhysicalAddress pa_read_ptr, pa_write_ptr;
	u32 target_addr;
	int size;
	int nread;
	u32 bs_va_startaddr = dec->virt_bsbuf_addr;
	u32 bs_va_endaddr = dec->virt_bsbuf_addr + STREAM_BUF_SIZE;
	u32 bs_pa_startaddr = dec->phy_bsbuf_addr;
	int space = 0;
	u32 room;
	unsigned char* ptr = NULL;
	*eos = 0;

	ret = vpu_DecGetBitstreamBuffer(handle, &pa_read_ptr, &pa_write_ptr, &space);
	if(ret != RETCODE_SUCCESS) return 0;

	target_addr = bs_va_startaddr + (pa_write_ptr - bs_pa_startaddr);


	/* Decoder bitstream buffer is empty */
	if(space <= 0)
		return 0;


	memset(dec->buffer, 0, MJPG_BUFFER_SIZE);

	while(!(ptr = queue_pop(dec->input_queue)))
	{
		usleep(0);
	}


	memcpy(dec->buffer, ptr, 262144);

	if(nread == -1) return 0;

	nread = MJPG_BUFFER_SIZE;

	if(nread>space)
	{
		return 0;
	}

	if((target_addr + nread) > bs_va_endaddr)
	{
		room = bs_va_endaddr - target_addr;
		memcpy((void*)target_addr, dec->buffer, room);
		memcpy((void*)bs_va_startaddr, dec->buffer+room, nread - room);
	}
	else
	{
		memcpy((void*)target_addr, dec->buffer, nread);
	}

	vpu_DecUpdateBitstreamBuffer(handle, nread);

	return nread;
}

static int decoder_open(DecodingInstance dec)
{
	RetCode ret;
	DecHandle handle = NULL;
	DecOpenParam oparam = {};

	oparam.bitstreamFormat = STD_MJPG;
	oparam.bitstreamBuffer = dec->phy_bsbuf_addr;
	oparam.bitstreamBufferSize = STREAM_BUF_SIZE;
	oparam.reorderEnable = dec->reorderEnable;
	oparam.mp4DeblkEnable = 0;
	oparam.chromaInterleave = 0;
	oparam.mp4Class = 0;
	oparam.mjpg_thumbNailDecEnable = 0;
	oparam.psSaveBuffer = dec->phy_ps_buf;
	oparam.psSaveBufferSize = PS_SAVE_SIZE;

	ret = vpu_DecOpen(&handle, &oparam);
	if (ret != RETCODE_SUCCESS) {
		fputs("vpu_DecOpen failed\n", stderr);
		return -1;
	}

	dec->handle = handle;
	return 0;
}

static int decoder_parse(DecodingInstance dec)
{
	DecInitialInfo initinfo = {0};
	DecHandle handle = dec->handle;
	int align, extended_fbcount;
	RetCode ret;
	char *count;


	ret = vpu_DecGiveCommand(handle, DEC_SET_REPORT_USERDATA, &dec->userData);
	if(ret != RETCODE_SUCCESS)
	{
		fprintf(stderr, "Failed to set user data report, ret %d\n", ret);
		return -1;
	}


	vpu_DecSetEscSeqInit(handle, 1);
	ret = vpu_DecGetInitialInfo(handle, &initinfo);
	vpu_DecSetEscSeqInit(handle, 0);

	if (ret != RETCODE_SUCCESS)
	{
		fprintf(stderr, "vpu_DecGetInitialInfo failed, ret:%d, errorcode:%lu\n", ret, initinfo.errorcode);
		return -1;
	}

	if(initinfo.streamInfoObtained)
	{
		dec->mjpg_fmt = initinfo.mjpg_sourceFormat;
	}

	dec->minFrameBufferCount = initinfo.minFrameBufferCount;

	count = getenv("VPU_EXTENDED_BUFFER_COUNT");
	if(count)
		extended_fbcount = atoi(count);
	else
		extended_fbcount = 2;

	dec->fbcount = initinfo.minFrameBufferCount + extended_fbcount;

	dec->picwidth = ((initinfo.picWidth + 15) & ~15);
	align = 16;
	dec->picheight = ((initinfo.picHeight + align - 1) & ~(align - 1));

	if ((dec->picwidth == 0) || (dec->picheight == 0))
		return -1;

	if((dec->picwidth > initinfo.picWidth || dec->picheight > initinfo.picHeight)
			&& (!initinfo.picCropRect.left && !initinfo.picCropRect.top && !initinfo.picCropRect.right
					&& !initinfo.picCropRect.bottom))
	{
		initinfo.picCropRect.left = 0;
		initinfo.picCropRect.top = 0;
		initinfo.picCropRect.right = initinfo.picWidth;
		initinfo.picCropRect.bottom = initinfo.picHeight;
	}


	memcpy(&(dec->picCropRect), &(initinfo.picCropRect), sizeof(initinfo.picCropRect));

	dec->phy_slicebuf_size = initinfo.worstSliceSize * 1024;
	dec->stride = dec->picwidth;

	return 0;
}

static int decoder_allocate_framebuffer(DecodingInstance dec)
{
	DecBufInfo bufinfo;
	int i, fbcount = dec->fbcount, totalfb, img_size;
	RetCode ret;
	DecHandle handle = dec->handle;
	FrameBuffer *fb;
	struct frame_buf **pfbpool;
	struct vpu_display *disp = NULL;
	int stride, divX, divY;
	vpu_mem_desc *mvcol_md = NULL;
	struct rot rotation = {};


	totalfb = fbcount + dec->extrafb;

	fb = dec->fb = calloc(totalfb, sizeof(FrameBuffer));

	pfbpool = dec->pfbpool = calloc(totalfb, sizeof(struct frame_buf *));


	disp = v4l_display_open(dec, totalfb, 1024, 768, 0, 0);

	if (disp == NULL) {
		goto err;
	}

	divX = (dec->mjpg_fmt == MODE420 || dec->mjpg_fmt == MODE422) ? 2 : 1;
	divY = (dec->mjpg_fmt == MODE420 || dec->mjpg_fmt == MODE224) ? 2 : 1;

	img_size = dec->stride * dec->picheight;
	dec->buffer_size = img_size * (dec->mjpg_fmt == MODE422 ? 2.0 : 1.5);


	mvcol_md = dec->mvcol_memdesc = calloc(totalfb, sizeof(vpu_mem_desc));

	for(i = 0; i < totalfb; i++)
	{
		fb[i].bufY = disp->buffers[i]->offset;
		fb[i].bufCb = fb[i].bufY + img_size;
		fb[i].bufCr = fb[i].bufCb + (img_size / divX / divY);
		/* allocate MvCol buffer here */
		memset(&mvcol_md[i], 0, sizeof(vpu_mem_desc));
		mvcol_md[i].size = img_size / divX / divY;
		IOGetPhyMem(&mvcol_md[i]);
		fb[i].bufMvCol = mvcol_md[i].phy_addr;

	}

	stride = ((dec->stride + 15) & ~15);
	bufinfo.avcSliceBufInfo.sliceSaveBuffer = dec->phy_slice_buf;
	bufinfo.avcSliceBufInfo.sliceSaveBufferSize = dec->phy_slicebuf_size;

	bufinfo.maxDecFrmInfo.maxMbX = dec->stride / 16;
	bufinfo.maxDecFrmInfo.maxMbY = dec->picheight / 16;
	bufinfo.maxDecFrmInfo.maxMbNum = dec->stride * dec->picheight / 256;
	ret = vpu_DecRegisterFrameBuffer(handle, fb, fbcount, stride, &bufinfo);

	if(ret != RETCODE_SUCCESS)
	{
		goto err1;
	}

	dec->disp = disp;
	return 0;

err1:
	v4l_display_close(disp);
	dec->disp = NULL;
err:
	free(dec->fb);
	free(dec->pfbpool);
	dec->fb = NULL;
	dec->pfbpool = NULL;
	return -1;
}

DecodingInstance vpu_create_decoding_instance_for_v4l2(queue input)
{
	DecodingInstance dec = NULL;
	DecParam decparam = {};
	int rot_stride = 0, fwidth, fheight, rot_angle = 0, mirror = 0;
	int rotid = 0;
	int eos = 0, fill_end_bs = 0;


	dec = calloc(1, sizeof(struct DecodingInstance));

	dec->mem_desc.size = STREAM_BUF_SIZE;
	IOGetPhyMem(&dec->mem_desc);
	IOGetVirtMem(&dec->mem_desc);

	dec->phy_bsbuf_addr = dec->mem_desc.phy_addr;
	dec->virt_bsbuf_addr = dec->mem_desc.virt_uaddr;
	dec->reorderEnable = 1;
	dec->input_queue = input;
	dec->buffer = calloc(1, MJPG_BUFFER_SIZE);

	decoder_open(dec);
	//dec_fill_bsbuffer(dec, fillsize, &eos, &fill_end_bs);
	fill_bsbuffer(dec, 1, &eos, &fill_end_bs);
	decoder_parse(dec);

	decoder_allocate_framebuffer(dec);

	rotid = 0;

	decparam.dispReorderBuf = 0;
	decparam.prescanEnable = 0;
	decparam.prescanMode = 0;
	decparam.skipframeMode = 0;
	decparam.skipframeNum = 0;
	decparam.iframeSearchEnable = 0;

	fwidth = ((dec->picwidth + 15) & ~15);
	fheight = ((dec->picheight + 15) & ~15);
	rot_stride = fwidth;

	vpu_DecGiveCommand(dec->handle, SET_ROTATION_ANGLE, &rot_angle);
	vpu_DecGiveCommand(dec->handle, SET_MIRROR_DIRECTION, &mirror);
	vpu_DecGiveCommand(dec->handle, SET_ROTATOR_STRIDE, &rot_stride);


	//img_size = dec->picwidth * dec->picheight * 3 / 2;

	return dec;
}

int vpu_decode_one_frame(DecodingInstance dec, unsigned char** output)
{
	DecHandle handle = dec->handle;
	FrameBuffer *fb = dec->fb;
	DecOutputInfo outinfo = {};
	static int rotid = 0;
	int ret;
	int is_waited_int;
	int loop_id;
	double frame_id = 0;
	int disp_clr_index = -1, actual_display_index = -1;
	struct frame_buf **pfbpool = dec->pfbpool;
	struct frame_buf *pfb = NULL;
	unsigned char* yuv_addr;
	int decIndex = 0;
	int err = 0, eos = 0, fill_end_bs = 0, decodefinish = 0;
	struct vpu_display *disp = dec->disp;
	int field = V4L2_FIELD_NONE;


	vpu_DecGiveCommand(handle, SET_ROTATOR_OUTPUT, (void *)&fb[rotid]);

	ret = vpu_DecStartOneFrame(handle, &(dec->decparam));

	if(ret != RETCODE_SUCCESS)
	{
		fputs("DecStartOneFrame failed\n", stderr);
		return -1;
	}

	is_waited_int = 0;
	loop_id = 0;


	while(vpu_IsBusy())
	{
		//err = dec_fill_bsbuffer(dec, STREAM_FILL_SIZE, &eos, &fill_end_bs);

		timer_start;
		err = fill_bsbuffer(dec, 0, &eos, &fill_end_bs);
		timer_stop;

		if(err < 0)
		{
			fputs("dec_fill_bsbuffer failed\n", stderr);
			return -1;
		}

		if(loop_id == 10)
		{
			err = vpu_SWReset(handle, 0);
			return -1;
		}

		if(!err)
		{
			vpu_WaitForInt(500);
			is_waited_int = 1;
			loop_id++;
		}
	}

	if(!is_waited_int)
		vpu_WaitForInt(500);
	ret = vpu_DecGetOutputInfo(handle, &outinfo);


	if(outinfo.indexFrameDisplay == 0)
	{
		outinfo.indexFrameDisplay = rotid;
	}

	dprintf(4, "frame_id = %d\n", (int)frame_id);
	if(ret != RETCODE_SUCCESS)
	{
		fprintf(stderr, "vpu_DecGetOutputInfo failed Err code is %d\n\tframe_id = %d\n", ret, (int)frame_id);
		return -1;
	}

	if(outinfo.decodingSuccess == 0)
	{
		fprintf(stderr, "Incomplete finish of decoding process.\n\tframe_id = %d\n", (int)frame_id);
		return -1;
	}

	if(outinfo.notSufficientPsBuffer)
	{
		fputs("PS Buffer overflow\n", stderr);
		return -1;
	}

	if(outinfo.notSufficientSliceBuffer)
	{
		fputs("Slice Buffer overflow\n", stderr);
		return -1;
	}

	if(outinfo.indexFrameDecoded >= 0)
		decIndex++;

	if(outinfo.indexFrameDisplay == -1)
		decodefinish = 1;
	else if((outinfo.indexFrameDisplay > dec->fbcount) && (outinfo.prescanresult != 0))
		decodefinish = 1;

	if(decodefinish)
		return -1;

	if((outinfo.indexFrameDisplay == -3) || (outinfo.indexFrameDisplay == -2))
	{
		dprintf(3, "VPU doesn't have picture to be displayed.\n\toutinfo.indexFrameDisplay = %d\n", outinfo.indexFrameDisplay);

		disp_clr_index = outinfo.indexFrameDisplay;

		return 0;
	}


	actual_display_index = rotid;
//	actual_display_index = 0;

//
//	{
//		pfb = pfbpool[actual_display_index];
//
//		yuv_addr = (unsigned char*)(pfb->addrY + pfb->desc.virt_uaddr - pfb->desc.phy_addr);
//
//		/* Memory copying on physical address is slow */
//		//memcpy(output, (unsigned char*)yuv_addr, img_size);
//		*output = yuv_addr;
//
//		disp_clr_index = outinfo.indexFrameDisplay;
//	}
//
	//fprintf(stderr, "memcpy %d bytes from 0x%X\n", disp->buffers[actual_display_index]->length, disp->buffers[actual_display_index]->start);
	//memcpy(*output, disp->buffers[actual_display_index]->start, disp->buffers[actual_display_index]->length);
	*output = disp->buffers[actual_display_index]->start;
//	v4l_put_data(disp, actual_display_index, field, 25);
//	fprintf(stderr, "%d\n", rotid);

	rotid++;
	rotid %= dec->fbcount;
	dec->rot_buf_count = rotid;
	frame_id++;
	return 0;
}

void vpu_display(DecodingInstance dec)
{
	v4l_put_data(dec->disp, dec->rot_buf_count, V4L2_FIELD_NONE, 30);
}


static int vpu_decoding_thread(DecodingInstance dec)
{
	int ret;
	unsigned char* frame = NULL;

	dec->run_thread = 1;
	while(dec->run_thread)
	{
		ret = vpu_decode_one_frame(dec, &frame);
		if(ret == -1) continue;
		queue_push(dec->output_queue, frame);
		vpu_display(dec);
	}
}

void vpu_start_decoding(DecodingInstance dec)
{
	fprintf(stderr, "dec->buffer_size == %d\n", dec->buffer_size);
	dec->output_queue = queue_new(dec->buffer_size, VPU_DECODING_QUEUE_SIZE);
	pthread_create(&dec->thread, NULL, vpu_decoding_thread, dec);
}

void vpu_stop_decoding(DecodingInstance dec)
{
	int ret;
	dec->run_thread = 0;
	pthread_join(dec->thread, &ret);
	queue_delete(&dec->output_queue);
}

queue vpu_get_decode_queue(DecodingInstance dec)
{
	return dec->output_queue;
}
