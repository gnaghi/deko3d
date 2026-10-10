#include "../dk_device.h"
#include "../cmdbuf_writer.h"

#include "engine_3d.h"

using namespace maxwell;
using namespace dk::detail;

using E = Engine3D;
using R = Engine3D::SetReportSemaphore;

// Transform feedback (stream output), programmed like nouveau nvc0
// (nvc0_tfb_validate, nvc0_so_target_save_offset, nvc0_draw_stream_output).
//
// Saved offsets are DK_NUM_TRANSFORM_FEEDBACK_BUFFERS counter reports of 16 bytes,
// one per buffer. The current write offset of a buffer is the 32-bit word
// s_tfbOffsetWord of its report: nouveau reads it at byte 4 (nvc0_query_hw.c,
// "u32 sequence, u32 count, u64 time"), whereas Primer.md describes counter
// reports as "u64 counter, u64 timestamp". To be confirmed on hardware
// (deko3d_review/tests/tfb_tests.c, test "report").

namespace
{
	constexpr uint32_t s_tfbOffsetWord = 1;

	constexpr DkGpuAddr offsetWordAddr(DkGpuAddr savedOffsets, uint32_t bufferId)
	{
		return savedOffsets + 16*bufferId + 4*s_tfbOffsetWord;
	}
}

void dkCmdBufBindTransformFeedbackLayouts(DkCmdBuf obj, DkTransformFeedbackLayout const* const layouts[], uint32_t numLayouts)
{
	DK_ENTRYPOINT(obj);
	DK_DEBUG_BAD_INPUT(numLayouts > DK_NUM_TRANSFORM_FEEDBACK_BUFFERS);
	DK_DEBUG_NON_NULL_ARRAY(layouts, numLayouts);
	CmdBufWriter w{obj};
	w.reserve(DK_NUM_TRANSFORM_FEEDBACK_BUFFERS * (4 + 1 + DK_MAX_TRANSFORM_FEEDBACK_VARYINGS/4));

	for (uint32_t i = 0; i < DK_NUM_TRANSFORM_FEEDBACK_BUFFERS; i ++)
	{
		DkTransformFeedbackLayout const* layout = i < numLayouts ? layouts[i] : nullptr;
		if (!layout || !layout->numVaryings)
		{
			// The buffer captures nothing
			w << CmdInline(3D, TfbLayout::VaryingCount{i}, 0);
			continue;
		}

		DK_DEBUG_BAD_INPUT(layout->numVaryings > DK_MAX_TRANSFORM_FEEDBACK_VARYINGS);
		DK_DEBUG_SIZE_ALIGN(layout->stride, 4);
		w << Cmd(3D, TfbLayout::Stream{i}, layout->stream, layout->numVaryings, layout->stride);

		// One byte per captured word, four per register; the unused bytes of the
		// last register are zero (as nouveau does)
		uint32_t numWords = (layout->numVaryings + 3) / 4;
		uint32_t words[DK_MAX_TRANSFORM_FEEDBACK_VARYINGS/4] = {};
		for (uint32_t j = 0; j < layout->numVaryings; j ++)
			words[j/4] |= uint32_t(layout->varyings[j]) << (8*(j%4));
		w << CmdList<1>{ MakeCmdHeader(Increasing, numWords, Subchannel3D, E::TfbVaryingLocs{} + 32*i) };
		w.addRawData(words, 4*numWords);
	}
}

void dkCmdBufBindTransformFeedbackBuffers(DkCmdBuf obj, uint32_t firstId, DkBufExtents const buffers[], uint32_t numBuffers)
{
	DK_ENTRYPOINT(obj);
	DK_DEBUG_BAD_INPUT(firstId > DK_NUM_TRANSFORM_FEEDBACK_BUFFERS || numBuffers > DK_NUM_TRANSFORM_FEEDBACK_BUFFERS || (firstId+numBuffers) > DK_NUM_TRANSFORM_FEEDBACK_BUFFERS);
	DK_DEBUG_NON_NULL_ARRAY(buffers, numBuffers);
	CmdBufWriter w{obj};
	w.reserve(5*numBuffers);

	for (uint32_t i = 0; i < numBuffers; i ++)
	{
		DkBufExtents const& buf = buffers[i];
		uint32_t id = firstId + i;
		if (!buf.size)
		{
			w << CmdInline(3D, TfbBuffer::Enable{id}, 0);
			continue;
		}

		DK_DEBUG_DATA_ALIGN(buf.addr, DK_TRANSFORM_FEEDBACK_BUF_ALIGNMENT);
		w << Cmd(3D, TfbBuffer::Enable{id}, 1, Iova(buf.addr), buf.size);
	}
}

void dkCmdBufBeginTransformFeedback(DkCmdBuf obj)
{
	DK_ENTRYPOINT(obj);
	CmdBufWriter w{obj};
	w.reserve(2*DK_NUM_TRANSFORM_FEEDBACK_BUFFERS + 1);

	// New capture: every buffer is written from its start
	for (uint32_t i = 0; i < DK_NUM_TRANSFORM_FEEDBACK_BUFFERS; i ++)
		w << Cmd(3D, TfbBuffer::Offset{i}, 0);
	w << CmdInline(3D, TfbEnable{}, 1);
}

void dkCmdBufEndTransformFeedback(DkCmdBuf obj)
{
	DK_ENTRYPOINT(obj);
	CmdBufWriter w{obj};
	w.reserve(1);

	w << CmdInline(3D, TfbEnable{}, 0);
}

void dkCmdBufSaveTransformFeedbackOffsets(DkCmdBuf obj, DkGpuAddr savedOffsets)
{
	DK_ENTRYPOINT(obj);
	DK_DEBUG_BAD_INPUT(savedOffsets == DK_GPU_ADDR_INVALID);
	DK_DEBUG_DATA_ALIGN(savedOffsets, 16);
	{
		CmdBufWriter w{obj};
		w.reserve(1 + 5*DK_NUM_TRANSFORM_FEEDBACK_BUFFERS + 1);

		// The offsets are only final once the stream output unit is idle (nouveau: SERIALIZE)
		w << CmdInline(3D, WaitForIdle{}, 0);
		for (uint32_t i = 0; i < DK_NUM_TRANSFORM_FEEDBACK_BUFFERS; i ++)
			w << Cmd(3D, SetReportSemaphoreOffset{}, Iova(savedOffsets + 16*i), 0,
				R::Operation::Counter |
				R::Unit::StrmOut |
				R::Counter::TransformFeedbackOffset |
				R::Index{i} |
				R::StructureSize::FourWords
			);
		w << CmdInline(3D, TiledCacheFlush{}, 0);
	}

	// Resume and DrawTransformFeedback fetch the offsets as command data (gpfifo
	// entries): stop the command processor from prefetching anything further
	// before the reports have landed (see dkCmdBufBarrier).
	dkCmdBufBarrier(obj, DkBarrier_Full, 0);
}

void dkCmdBufResumeTransformFeedback(DkCmdBuf obj, DkGpuAddr savedOffsets)
{
	DK_ENTRYPOINT(obj);
	DK_DEBUG_BAD_INPUT(savedOffsets == DK_GPU_ADDR_INVALID);
	DK_DEBUG_DATA_ALIGN(savedOffsets, 16);
	CmdBufWriter w{obj};

	// Each buffer continues at its saved offset, which the GPU reads itself: the
	// method header is in the command list and its data word is the report
	// (same technique as the indirect draws)
	for (uint32_t i = 0; i < DK_NUM_TRANSFORM_FEEDBACK_BUFFERS; i ++)
	{
		w.reserve(1);
		w << CmdList<1>{ MakeCmdHeader(Increasing, 1, Subchannel3D, E::TfbBuffer::Offset{i}) };
		w.split(CtrlCmdGpfifoEntry::NoPrefetch);
		w.addRaw(offsetWordAddr(savedOffsets, i), 1, CtrlCmdGpfifoEntry::AutoKick);
	}

	w.reserve(1);
	w << CmdInline(3D, TfbEnable{}, 1);
}

void dkCmdBufDrawTransformFeedback(DkCmdBuf obj, DkPrimitive prim, uint32_t stride, DkGpuAddr savedOffsets, uint32_t bufferId)
{
	DK_ENTRYPOINT(obj);
	DK_DEBUG_BAD_INPUT(savedOffsets == DK_GPU_ADDR_INVALID);
	DK_DEBUG_DATA_ALIGN(savedOffsets, 16);
	DK_DEBUG_BAD_INPUT(bufferId >= DK_NUM_TRANSFORM_FEEDBACK_BUFFERS);
	DK_DEBUG_BAD_INPUT(stride == 0 || stride > 0xFFF);
	CmdBufWriter w{obj};
	w.reserve(9);

	// Vertex count = bytes written to the buffer / stride, computed by the GPU
	// from the saved offset of the buffer (nouveau nvc0_draw_stream_output).
	// nouveau does not reset DrawArraysFirst, left over from the last
	// dkCmdBufDraw: clear it in case the hardware adds it to the first vertex.
	w << Cmd(3D, DrawArraysFirst{}, 0);
	w << Cmd(3D, VertexBeginGl{}, prim);
	w << Cmd(3D, DrawTfbBase{}, 0);
	w << Cmd(3D, DrawTfbStride{}, stride);
	w << CmdList<1>{ MakeCmdHeader(Increasing, 1, Subchannel3D, E::DrawTfbBytes{}) };
	w.split(CtrlCmdGpfifoEntry::NoPrefetch);
	w.addRaw(offsetWordAddr(savedOffsets, bufferId), 1, CtrlCmdGpfifoEntry::AutoKick);

	w.reserve(1);
	w << CmdInline(3D, VertexEndGl{}, 0);
}
