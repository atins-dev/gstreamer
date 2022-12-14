/*
 * GStreamer
 * Copyright (C) 2022 Seungha Yang <seungha@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>

#include <windows.h>

static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE cond = CONDITION_VARIABLE_INIT;
static bool pipeline_running = false;
static bool shutdown_pipeline = false;

static LRESULT CALLBACK
window_proc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message) {
    case WM_DESTROY:
      gst_println ("Destory");
      return 0;
    default:
      break;
  }

  return DefWindowProc (hwnd, message, wParam, lParam);
}

static GstPadProbeReturn
buffer_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  gst_println ("Got first buffer");

  AcquireSRWLockExclusive (&lock);
  pipeline_running = true;
  WakeAllConditionVariable (&cond);
  ReleaseSRWLockExclusive (&lock);

  return GST_PAD_PROBE_REMOVE;
}

static gpointer
pipeline_thread_func (HWND hwnd)
{
  GstElement *pipeline;
  GstElement *sink;
  GstPad *pad;

  pipeline = gst_parse_launch ("videotestsrc ! queue ! "
      "d3d11videosink name=sink", nullptr);
  sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (sink),
      (guintptr) hwnd);
  gst_object_unref (sink);

  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe_cb,
      nullptr, nullptr);

  gst_println ("%p Starting test pipeline", g_thread_self ());

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  AcquireSRWLockExclusive (&lock);
  while (!shutdown_pipeline)
    SleepConditionVariableSRW (&cond, &lock, INFINITE, 0);
  ReleaseSRWLockExclusive (&lock);

  gst_println ("Shuting down pipeline");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_println ("Shutting down done");

  gst_object_unref (pipeline);

  return nullptr;
}

static gpointer
sleep_thread_func (HWND hwnd)
{
  /* Sleep 1 second */
  Sleep (1000);

  gst_println ("Triggering pipeline launch");
  PostMessageA (hwnd, WM_USER, 0, 0);

  return nullptr;
}

gint
main (gint argc, gchar ** argv)
{
  WNDCLASSEXA wc = { 0, };
  RECT wr = { 0, 0, 320, 240 };
  HWND hwnd;
  GThread *thread;
  MSG msg;

  gst_init (nullptr, nullptr);

  /* prepare window */
  wc.cbSize = sizeof (WNDCLASSEXA);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = (WNDPROC) window_proc;
  wc.hInstance = GetModuleHandle (nullptr);
  wc.hCursor = LoadCursor (NULL, IDC_ARROW);
  wc.lpszClassName = "GstD3D11VideoSinkExample";
  RegisterClassExA (&wc);

  AdjustWindowRect (&wr, WS_OVERLAPPEDWINDOW, FALSE);

  hwnd = CreateWindowExA (0, wc.lpszClassName, "GstD3D11VideoSinkExample",
      WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr,
      GetModuleHandle (nullptr), nullptr);

  thread = g_thread_new ("sleep-thread", (GThreadFunc) sleep_thread_func,
      hwnd);

  while (true) {
    if (PeekMessage (&msg, hwnd, 0, 0, PM_REMOVE)) {
      TranslateMessage (&msg);

      gst_println ("%14p Got message 0x%x", g_thread_self (), msg.message);

      if (msg.message == WM_USER) {
        gst_println ("Got pipeline launch message");
        g_thread_join (thread);

        thread = g_thread_new ("pipeline-thread",
            (GThreadFunc) pipeline_thread_func, hwnd);

        AcquireSRWLockExclusive (&lock);
        gst_println ("Wait for buffer");
        while (!pipeline_running)
          SleepConditionVariableSRW (&cond, &lock, INFINITE, 0);
        gst_println ("Pipeline is running now");

        Sleep (1000);

        gst_println ("Sleep done");
        shutdown_pipeline = true;
        WakeAllConditionVariable (&cond);
        ReleaseSRWLockExclusive (&lock);

        gst_println ("Waiting for pipeline thread join");
        g_thread_join (thread);
        gst_println ("pipeline thread joined");
        break;
      }
      DispatchMessage (&msg);
    }
  }

  DestroyWindow (hwnd);

  return 0;
}
