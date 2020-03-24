// 包含头文件
#include <tchar.h>
#include "vdev.h"
#include "libavformat/avformat.h"

// 内部常量定义
#define DEF_VDEV_BUF_NUM  3

// 内部类型定义
typedef struct {
    // common members
    VDEV_COMMON_MEMBERS

    HDC      hdcsrc;
    HDC      hdcdst;
    HBITMAP *hbitmaps;
    BYTE   **pbmpbufs;
    HFONT    hfont;
} VDEVGDICTXT;

// 内部函数实现
static void* video_render_thread_proc(void *param)
{
    VDEVGDICTXT  *c = (VDEVGDICTXT*)param;
    LOGFONT logfont = {0};
    HFONT     hfont = NULL;

    while (!(c->status & VDEV_CLOSE)) {
        pthread_mutex_lock(&c->mutex);
        while (c->size <= 0 && (c->status & VDEV_CLOSE) == 0) pthread_cond_wait(&c->cond, &c->mutex);
        if (c->size > 0) {
            c->size--;
            if (vdev_refresh_background(c) && c->ppts[c->head] != -1) {
                SelectObject(c->hdcsrc, c->hbitmaps[c->head]);
                if (c->textt[0]) {
                    if (c->status & VDEV_CONFIG_FONT) {
                        c->status &= ~VDEV_CONFIG_FONT;
                        _tcscpy_s(logfont.lfFaceName, _countof(logfont.lfFaceName), c->font_name);
                        logfont.lfHeight = c->font_size;
                        hfont = CreateFontIndirect(&logfont);
                        SelectObject(c->hdcsrc, hfont);
                        if (c->hfont) DeleteObject(c->hfont);
                        c->hfont = hfont;
                    }
                    SetTextColor(c->hdcsrc, c->textc);
                    TextOut(c->hdcsrc, c->textx, c->texty, c->textt, (int)_tcslen(c->textt));
                }
                BitBlt(c->hdcdst, c->x, c->y, c->w, c->h, c->hdcsrc, 0, 0, SRCCOPY);
                c->cmnvars->vpts = c->ppts[c->head];
                av_log(NULL, AV_LOG_INFO, "vpts: %lld\n", c->cmnvars->vpts);
            }
            if (++c->head == c->bufnum) c->head = 0;
            pthread_cond_signal(&c->cond);
        }
        pthread_mutex_unlock(&c->mutex);

        // handle av-sync & frame rate & complete
        vdev_avsync_and_complete(c);
    }

    return NULL;
}

static void vdev_gdi_lock(void *ctxt, int64_t pts, uint8_t *buffer[8], int linesize[8])
{
    VDEVGDICTXT *c       = (VDEVGDICTXT*)ctxt;
    int          bmpw    =  0;
    int          bmph    =  0;
    BITMAPINFO   bmpinfo = {0};
    BITMAP       bitmap;

    pthread_mutex_lock(&c->mutex);
    while (c->size >= c->bufnum && (c->status & VDEV_CLOSE) == 0) pthread_cond_wait(&c->cond, &c->mutex);
    if (c->size < c->bufnum) {
        c->ppts[c->tail] = pts;
        if (c->hbitmaps[c->tail]) {
            GetObject(c->hbitmaps[c->tail], sizeof(BITMAP), &bitmap);
            bmpw = bitmap.bmWidth ;
            bmph = bitmap.bmHeight;
        }

        if (bmpw != c->w || bmph != c->h) {
            c->sw = c->w; c->sh = c->h;
            if (c->hbitmaps[c->tail]) {
                DeleteObject(c->hbitmaps[c->tail]);
            }

            bmpinfo.bmiHeader.biSize        =  sizeof(BITMAPINFOHEADER);
            bmpinfo.bmiHeader.biWidth       =  c->w;
            bmpinfo.bmiHeader.biHeight      = -c->h;
            bmpinfo.bmiHeader.biPlanes      =  1;
            bmpinfo.bmiHeader.biBitCount    =  32;
            bmpinfo.bmiHeader.biCompression =  BI_RGB;
            c->hbitmaps[c->tail] = CreateDIBSection(c->hdcsrc, &bmpinfo, DIB_RGB_COLORS,
                                            (void**)&c->pbmpbufs[c->tail], NULL, 0);
            GetObject(c->hbitmaps[c->tail], sizeof(BITMAP), &bitmap);
        }

        if (buffer  ) buffer[0]   = c->pbmpbufs[c->tail];
        if (linesize) linesize[0] = bitmap.bmWidthBytes ;
        if (++c->tail == c->bufnum) c->tail = 0;
        c->size++;
        pthread_cond_signal(&c->cond);
    }
}

static void vdev_gdi_unlock(void *ctxt)
{
    VDEVGDICTXT *c = (VDEVGDICTXT*)ctxt;
    pthread_mutex_unlock(&c->mutex);
}

static void vdev_gdi_setrect(void *ctxt, int x, int y, int w, int h)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    c->sw = w; c->sh = h;
}

static void vdev_gdi_destroy(void *ctxt)
{
    int i;
    VDEVGDICTXT *c = (VDEVGDICTXT*)ctxt;

    //++ for video
    DeleteDC (c->hdcsrc);
    ReleaseDC((HWND)c->surface, c->hdcdst);
    for (i=0; i<c->bufnum; i++) {
        if (c->hbitmaps[i]) {
            DeleteObject(c->hbitmaps[i]);
        }
    }
    //-- for video

    // delete font
    DeleteObject(c->hfont);

    pthread_mutex_destroy(&c->mutex);
    pthread_cond_destroy (&c->cond );

    // free memory
    free(c->ppts    );
    free(c->hbitmaps);
    free(c->pbmpbufs);
    free(c);
}

// 接口函数实现
void* vdev_gdi_create(void *surface, int bufnum)
{
    VDEVGDICTXT *ctxt = (VDEVGDICTXT*)calloc(1, sizeof(VDEVGDICTXT));
    if (!ctxt) return NULL;

    // init mutex & cond
    pthread_mutex_init(&ctxt->mutex, NULL);
    pthread_cond_init (&ctxt->cond , NULL);

    // init vdev context
    bufnum         = bufnum ? bufnum : DEF_VDEV_BUF_NUM;
    ctxt->bufnum   = bufnum;
    ctxt->pixfmt   = AV_PIX_FMT_RGB32;
    ctxt->lock     = vdev_gdi_lock;
    ctxt->unlock   = vdev_gdi_unlock;
    ctxt->setrect  = vdev_gdi_setrect;
    ctxt->destroy  = vdev_gdi_destroy;

    // alloc buffer & semaphore
    ctxt->ppts     = (int64_t*)calloc(bufnum, sizeof(int64_t));
    ctxt->hbitmaps = (HBITMAP*)calloc(bufnum, sizeof(HBITMAP));
    ctxt->pbmpbufs = (BYTE**  )calloc(bufnum, sizeof(BYTE*  ));

    ctxt->hdcdst = GetDC((HWND)surface);
    ctxt->hdcsrc = CreateCompatibleDC(ctxt->hdcdst);
    if (!ctxt->ppts || !ctxt->hbitmaps || !ctxt->pbmpbufs || !ctxt->mutex || !ctxt->cond || !ctxt->hdcdst || !ctxt->hdcsrc) {
        av_log(NULL, AV_LOG_ERROR, "failed to allocate resources for vdev-gdi !\n");
        exit(0);
    }
    SetBkMode(ctxt->hdcsrc, TRANSPARENT);

    // create video rendering thread
    pthread_create(&ctxt->thread, NULL, video_render_thread_proc, ctxt);
    return ctxt;
}
