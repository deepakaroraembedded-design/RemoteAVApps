/*
 * nv12_conv.cu — CUDA NV12 -> BGRA conversion with scaling.
 *
 * Replaces the CPU swscale step: the GPU converts the decoder's NV12 output
 * to BGRA (optionally scaling, e.g. 4K -> 1080p) in a single kernel.
 *
 * Build:
 *   nvcc -O3 -shared -Xcompiler -fPIC -o libnv12conv.so nv12_conv.cu
 *
 * Runtime API; links libcudart.
 */

#include <cuda.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>

/* BT.601 limited-range NV12 -> RGBA/BGRA. Output written as BGRA (byte order
 * B,G,R,0 matching the XRGB8888 framebuffer). */
__global__ void nv12_to_bgra_kernel(const unsigned char *__restrict__ y,
                                    const unsigned char *__restrict__ uv,
                                    int w, int h, int y_stride, int uv_stride,
                                    unsigned char *__restrict__ out,
                                    int out_w, int out_h, int out_stride) {
    const int ox = blockIdx.x * blockDim.x + threadIdx.x;
    const int oy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ox >= out_w || oy >= out_h) return;

    /* nearest-neighbour source coordinate */
    const int sx = (ox * w) / out_w;
    const int sy = (oy * h) / out_h;
    const int sx2 = sx / 2;
    const int sy2 = sy / 2;

    const int Y = (int)y[sy * y_stride + sx];
    const int U = (int)uv[sy2 * uv_stride + sx2 * 2];
    const int V = (int)uv[sy2 * uv_stride + sx2 * 2 + 1];

    int r = (int)(1.164f * (Y - 16) + 1.596f * (V - 128));
    int g = (int)(1.164f * (Y - 16) - 0.813f * (V - 128) - 0.392f * (U - 128));
    int b = (int)(1.164f * (Y - 16) + 2.017f * (U - 128));

    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);

    unsigned char *p = out + ((size_t)oy * out_stride + ox) * 4;
    p[0] = (unsigned char)b;   /* B */
    p[1] = (unsigned char)g;   /* G */
    p[2] = (unsigned char)r;   /* R */
    p[3] = 0;                  /* pad / A */
}

extern "C" {

/* --- Design A: host-NV12 -> host-BGRA (uploads + downloads) --- */
static unsigned char *d_y = NULL, *d_uv = NULL, *d_out = NULL;
static size_t d_y_cap = 0, d_uv_cap = 0, d_out_cap = 0;

int nv12_to_bgra(const unsigned char *y, const unsigned char *uv,
                 int w, int h, int y_stride, int uv_stride,
                 unsigned char *bgra, int out_w, int out_h) {
    static int inited = 0;
    if (!inited) {
        if (cudaSetDevice(0) != cudaSuccess) {
            fprintf(stderr, "nv12conv: cudaSetDevice failed\n");
            return -1;
        }
        inited = 1;
    }

    const size_t y_bytes = (size_t)y_stride * h;
    const size_t uv_bytes = (size_t)uv_stride * (h / 2);
    const size_t out_bytes = (size_t)out_w * out_h * 4;

    if (d_y_cap < y_bytes) {
        if (d_y) cudaFree(d_y);
        if (cudaMalloc(&d_y, y_bytes) != cudaSuccess) return -1;
        d_y_cap = y_bytes;
    }
    if (d_uv_cap < uv_bytes) {
        if (d_uv) cudaFree(d_uv);
        if (cudaMalloc(&d_uv, uv_bytes) != cudaSuccess) return -1;
        d_uv_cap = uv_bytes;
    }
    if (d_out_cap < out_bytes) {
        if (d_out) cudaFree(d_out);
        if (cudaMalloc(&d_out, out_bytes) != cudaSuccess) return -1;
        d_out_cap = out_bytes;
    }

    cudaMemcpyAsync(d_y, y, y_bytes, cudaMemcpyHostToDevice, 0);
    cudaMemcpyAsync(d_uv, uv, uv_bytes, cudaMemcpyHostToDevice, 0);

    dim3 block(16, 16);
    dim3 grid((out_w + 15) / 16, (out_h + 15) / 16);
    nv12_to_bgra_kernel<<<grid, block, 0, 0>>>(d_y, d_uv, w, h, y_stride,
                                               uv_stride, d_out, out_w, out_h,
                                               out_w);
    cudaMemcpyAsync(bgra, d_out, out_bytes, cudaMemcpyDeviceToHost, 0);
    cudaDeviceSynchronize();
    return 0;
}

/* --- Design B: device-NV12 -> host-BGRA (single D2H). -----------------
 * d_y/d_uv are CUDA device pointers (CUVID CUDA output); the kernel writes
 * to a device buffer, then one cudaMemcpy D2H deposits BGRA into the
 * caller's scanout memory (a DRM dumb buffer). */
static unsigned char *dbg_out = NULL;
static size_t dbg_cap = 0;

int nv12_to_bgra_device_out(const void *d_y, const void *d_uv,
                            int w, int h, int y_stride, int uv_stride,
                            unsigned char *host_bgra, int out_w, int out_h) {
    static int inited = 0;
    if (!inited) {
        if (cudaSetDevice(0) != cudaSuccess) return -1;
        inited = 1;
    }
    const size_t out_bytes = (size_t)out_w * out_h * 4;
    if (dbg_cap < out_bytes) {
        if (dbg_out) cudaFree(dbg_out);
        if (cudaMalloc(&dbg_out, out_bytes) != cudaSuccess) return -1;
        dbg_cap = out_bytes;
    }
    dim3 block(16, 16);
    dim3 grid((out_w + 15) / 16, (out_h + 15) / 16);
    nv12_to_bgra_kernel<<<grid, block, 0, 0>>>(
        (const unsigned char *)d_y, (const unsigned char *)d_uv,
        w, h, y_stride, uv_stride, dbg_out, out_w, out_h, out_w);
    cudaMemcpyAsync(host_bgra, dbg_out, out_bytes, cudaMemcpyDeviceToHost, 0);
    cudaDeviceSynchronize();
    return 0;
}

/* Import a dma-buf/prime fd into CUDA as a device pointer (NVIDIA OpaqueFd). */
#define MAX_IMPORTS 8
static cudaExternalMemory_t g_ext[MAX_IMPORTS];
static int g_next = 0;

int cuda_import_fd(int fd, size_t size, void **dev_ptr) {
    if (g_next >= MAX_IMPORTS) return -1;
    cudaExternalMemoryHandleDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.type = cudaExternalMemoryHandleTypeOpaqueFd;
    desc.handle.fd = fd;
    desc.size = size;
    desc.flags = 0;
    if (cudaImportExternalMemory(&g_ext[g_next], &desc) != cudaSuccess) {
        fprintf(stderr, "nv12conv: cudaImportExternalMemory failed: %s\n",
                cudaGetErrorString(cudaGetLastError()));
        return -1;
    }
    cudaExternalMemoryBufferDesc bufDesc;
    memset(&bufDesc, 0, sizeof(bufDesc));
    bufDesc.offset = 0;
    bufDesc.size = size;
    if (cudaExternalMemoryGetMappedBuffer(dev_ptr, g_ext[g_next], &bufDesc)
        != cudaSuccess) {
        fprintf(stderr, "nv12conv: map failed: %s\n",
                cudaGetErrorString(cudaGetLastError()));
        return -1;
    }
    g_next++;
    return 0;
}

/* Create the primary CUDA context up front so CUVID (opened later) and the
 * conversion share one context; primary contexts are thread-usable. */
int cuda_init(void) {
    if (cudaSetDevice(0) != cudaSuccess) return -1;
    cudaFree(0);   /* force context creation */
    return 0;
}

/* Make an arbitrary CUcontext current in the calling thread (driver API).
 * Used to bind the FFmpeg CUVID hw_device_ctx context in the decode thread. */
int cuda_set_current(void *ctx) {
    return (int)cuCtxSetCurrent((CUcontext)ctx);
}

/* --- Design B / Option 1: stream-overlapped conversion with pinned
 * staging. D2H lands in pinned host staging (truly async DMA); the CPU
 * then memcpy's it into the DRM scanout buffer (which can't be pinned). */
#define N_STAGE 3
static unsigned char *g_stage[N_STAGE] = {0, 0, 0};
static size_t g_stage_cap[N_STAGE] = {0, 0, 0};
static cudaStream_t g_conv_stream = 0;

int conv_async(const void *d_y, const void *d_uv, int w, int h, int y_stride,
               int uv_stride, int stage_idx, int out_w, int out_h,
               void **out_event) {
    static int inited = 0;
    if (!inited) {
        /* Operate in the CURRENT context (the caller binds CUVID's context);
         * device pointers are context-specific, so we must NOT switch. */
        if (cudaStreamCreate(&g_conv_stream) != cudaSuccess) return -1;
        inited = 1;
    }
    if (stage_idx < 0 || stage_idx >= N_STAGE) return -1;
    const size_t out_bytes = (size_t)out_w * out_h * 4;
    if (g_stage_cap[stage_idx] < out_bytes) {
        if (g_stage[stage_idx]) cudaFreeHost(g_stage[stage_idx]);
        if (cudaHostAlloc(&g_stage[stage_idx], out_bytes,
                          cudaHostAllocDefault) != cudaSuccess) return -1;
        g_stage_cap[stage_idx] = out_bytes;
    }
    if (dbg_cap < out_bytes) {
        if (dbg_out) cudaFree(dbg_out);
        if (cudaMalloc(&dbg_out, out_bytes) != cudaSuccess) return -1;
        dbg_cap = out_bytes;
    }
    dim3 block(16, 16);
    dim3 grid((out_w + 15) / 16, (out_h + 15) / 16);
    nv12_to_bgra_kernel<<<grid, block, 0, g_conv_stream>>>(
        (const unsigned char *)d_y, (const unsigned char *)d_uv,
        w, h, y_stride, uv_stride, dbg_out, out_w, out_h, out_w);
    cudaMemcpyAsync(g_stage[stage_idx], dbg_out, out_bytes,
                    cudaMemcpyDeviceToHost, g_conv_stream);
    if (out_event) {
        cudaEvent_t ev;
        if (cudaEventCreateWithFlags(&ev, 0) != cudaSuccess) return -1;
        if (cudaEventRecord(ev, g_conv_stream) != cudaSuccess) return -1;
        *out_event = (void *)ev;
    }
    return 0;
}

/* Pinned staging pointer for the given slot (CPU reads it after wait). */
const unsigned char *conv_stage_ptr(int stage_idx) {
    if (stage_idx < 0 || stage_idx >= N_STAGE) return NULL;
    return g_stage[stage_idx];
}

int conv_wait_event(void *event) {
    return (int)cudaEventSynchronize((cudaEvent_t)event);
}

void conv_free_event(void *event) {
    if (event) cudaEventDestroy((cudaEvent_t)event);
}

int conv_sync(void) {
    return cudaStreamSynchronize(g_conv_stream);
}

} /* extern "C" */
