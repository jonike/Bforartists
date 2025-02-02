/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup wm
 */

#include "BKE_callbacks.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_scene.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "DEG_depsgraph.h"

#include "DNA_camera_types.h"
#include "DNA_space_types.h"

#include "DRW_engine.h"

#include "ED_screen.h"

#include "GHOST_C-api.h"

#include "GPU_viewport.h"

#include "MEM_guardedalloc.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm_surface.h"
#include "wm_window.h"
#include "wm_xr_intern.h"

static wmSurface *g_xr_surface = NULL;
static CLG_LogRef LOG = {"wm.xr"};

/* -------------------------------------------------------------------- */

static void wm_xr_session_create_cb(void)
{
  Main *bmain = G_MAIN;
  wmWindowManager *wm = bmain->wm.first;
  wmXrData *xr_data = &wm->xr;

  /* Get action set data from Python. */
  BKE_callback_exec_null(bmain, BKE_CB_EVT_XR_SESSION_START_PRE);

  wm_xr_session_actions_init(xr_data);
}

static void wm_xr_session_controller_data_free(wmXrSessionState *state)
{
  BLI_freelistN(&state->controllers);
}

void wm_xr_session_data_free(wmXrSessionState *state)
{
  wm_xr_session_controller_data_free(state);
}

static void wm_xr_session_exit_cb(void *customdata)
{
  wmXrData *xr_data = customdata;
  if (!xr_data->runtime) {
    return;
  }

  xr_data->runtime->session_state.is_started = false;

  if (xr_data->runtime->exit_fn) {
    xr_data->runtime->exit_fn(xr_data);
  }

  /* Free the entire runtime data (including session state and context), to play safe. */
  wm_xr_runtime_data_free(&xr_data->runtime);
}

static void wm_xr_session_begin_info_create(wmXrData *xr_data,
                                            GHOST_XrSessionBeginInfo *r_begin_info)
{
  /* Callback for when the session is created. This is needed to create and bind OpenXR actions
   * after the session is created but before it is started. */
  r_begin_info->create_fn = wm_xr_session_create_cb;

  /* WM-XR exit function, does some own stuff and calls callback passed to wm_xr_session_toggle(),
   * to allow external code to execute its own session-exit logic. */
  r_begin_info->exit_fn = wm_xr_session_exit_cb;
  r_begin_info->exit_customdata = xr_data;
}

void wm_xr_session_toggle(wmWindowManager *wm,
                          wmWindow *session_root_win,
                          wmXrSessionExitFn session_exit_fn)
{
  wmXrData *xr_data = &wm->xr;

  if (WM_xr_session_exists(xr_data)) {
    GHOST_XrSessionEnd(xr_data->runtime->context);
    xr_data->runtime->session_state.is_started = false;
  }
  else {
    GHOST_XrSessionBeginInfo begin_info;

    xr_data->runtime->session_root_win = session_root_win;
    xr_data->runtime->session_state.is_started = true;
    xr_data->runtime->exit_fn = session_exit_fn;

    wm_xr_session_begin_info_create(xr_data, &begin_info);
    GHOST_XrSessionStart(xr_data->runtime->context, &begin_info);
  }
}

/**
 * Check if the XR-Session was triggered.
 * If an error happened while trying to start a session, this returns false too.
 */
bool WM_xr_session_exists(const wmXrData *xr)
{
  return xr->runtime && xr->runtime->context && xr->runtime->session_state.is_started;
}

void WM_xr_session_base_pose_reset(wmXrData *xr)
{
  xr->runtime->session_state.force_reset_to_base_pose = true;
}

/**
 * Check if the session is running, according to the OpenXR definition.
 */
bool WM_xr_session_is_ready(const wmXrData *xr)
{
  return WM_xr_session_exists(xr) && GHOST_XrSessionIsRunning(xr->runtime->context);
}

static void wm_xr_session_base_pose_calc(const Scene *scene,
                                         const XrSessionSettings *settings,
                                         GHOST_XrPose *r_base_pose)
{
  const Object *base_pose_object = ((settings->base_pose_type == XR_BASE_POSE_OBJECT) &&
                                    settings->base_pose_object) ?
                                       settings->base_pose_object :
                                       scene->camera;

  if (settings->base_pose_type == XR_BASE_POSE_CUSTOM) {
    float tmp_quatx[4], tmp_quatz[4];

    copy_v3_v3(r_base_pose->position, settings->base_pose_location);
    axis_angle_to_quat_single(tmp_quatx, 'X', M_PI_2);
    axis_angle_to_quat_single(tmp_quatz, 'Z', settings->base_pose_angle);
    mul_qt_qtqt(r_base_pose->orientation_quat, tmp_quatz, tmp_quatx);
  }
  else if (base_pose_object) {
    float tmp_quat[4];
    float tmp_eul[3];

    mat4_to_loc_quat(r_base_pose->position, tmp_quat, base_pose_object->obmat);

    /* Only use rotation around Z-axis to align view with floor. */
    quat_to_eul(tmp_eul, tmp_quat);
    tmp_eul[0] = M_PI_2;
    tmp_eul[1] = 0;
    eul_to_quat(r_base_pose->orientation_quat, tmp_eul);
  }
  else {
    copy_v3_fl(r_base_pose->position, 0.0f);
    axis_angle_to_quat_single(r_base_pose->orientation_quat, 'X', M_PI_2);
  }
}

static void wm_xr_session_draw_data_populate(wmXrData *xr_data,
                                             Scene *scene,
                                             Depsgraph *depsgraph,
                                             wmXrDrawData *r_draw_data)
{
  const XrSessionSettings *settings = &xr_data->session_settings;

  memset(r_draw_data, 0, sizeof(*r_draw_data));
  r_draw_data->scene = scene;
  r_draw_data->depsgraph = depsgraph;
  r_draw_data->xr_data = xr_data;
  r_draw_data->surface_data = g_xr_surface->customdata;

  wm_xr_session_base_pose_calc(r_draw_data->scene, settings, &r_draw_data->base_pose);
}

wmWindow *wm_xr_session_root_window_or_fallback_get(const wmWindowManager *wm,
                                                    const wmXrRuntimeData *runtime_data)
{
  if (runtime_data->session_root_win &&
      BLI_findindex(&wm->windows, runtime_data->session_root_win) != -1) {
    /* Root window is still valid, use it. */
    return runtime_data->session_root_win;
  }
  /* Otherwise, fallback. */
  return wm->windows.first;
}

/**
 * Get the scene and depsgraph shown in the VR session's root window (the window the session was
 * started from) if still available. If it's not available, use some fallback window.
 *
 * It's important that the VR session follows some existing window, otherwise it would need to have
 * an own depsgraph, which is an expense we should avoid.
 */
static void wm_xr_session_scene_and_evaluated_depsgraph_get(Main *bmain,
                                                            const wmWindowManager *wm,
                                                            Scene **r_scene,
                                                            Depsgraph **r_depsgraph)
{
  const wmWindow *root_win = wm_xr_session_root_window_or_fallback_get(wm, wm->xr.runtime);

  /* Follow the scene & view layer shown in the root 3D View. */
  Scene *scene = WM_window_get_active_scene(root_win);
  ViewLayer *view_layer = WM_window_get_active_view_layer(root_win);

  Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer);
  BLI_assert(scene && view_layer && depsgraph);
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
  *r_scene = scene;
  *r_depsgraph = depsgraph;
}

typedef enum wmXrSessionStateEvent {
  SESSION_STATE_EVENT_NONE = 0,
  SESSION_STATE_EVENT_START,
  SESSION_STATE_EVENT_RESET_TO_BASE_POSE,
  SESSION_STATE_EVENT_POSITION_TRACKING_TOGGLE,
} wmXrSessionStateEvent;

static bool wm_xr_session_draw_data_needs_reset_to_base_pose(const wmXrSessionState *state,
                                                             const XrSessionSettings *settings)
{
  if (state->force_reset_to_base_pose) {
    return true;
  }
  return ((settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) &&
         ((state->prev_base_pose_type != settings->base_pose_type) ||
          (state->prev_base_pose_object != settings->base_pose_object));
}

static wmXrSessionStateEvent wm_xr_session_state_to_event(const wmXrSessionState *state,
                                                          const XrSessionSettings *settings)
{
  if (!state->is_view_data_set) {
    return SESSION_STATE_EVENT_START;
  }
  if (wm_xr_session_draw_data_needs_reset_to_base_pose(state, settings)) {
    return SESSION_STATE_EVENT_RESET_TO_BASE_POSE;
  }

  const bool position_tracking_toggled = ((state->prev_settings_flag &
                                           XR_SESSION_USE_POSITION_TRACKING) !=
                                          (settings->flag & XR_SESSION_USE_POSITION_TRACKING));
  if (position_tracking_toggled) {
    return SESSION_STATE_EVENT_POSITION_TRACKING_TOGGLE;
  }

  return SESSION_STATE_EVENT_NONE;
}

void wm_xr_session_draw_data_update(const wmXrSessionState *state,
                                    const XrSessionSettings *settings,
                                    const GHOST_XrDrawViewInfo *draw_view,
                                    wmXrDrawData *draw_data)
{
  const wmXrSessionStateEvent event = wm_xr_session_state_to_event(state, settings);
  const bool use_position_tracking = (settings->flag & XR_SESSION_USE_POSITION_TRACKING);

  switch (event) {
    case SESSION_STATE_EVENT_START:
      if (use_position_tracking) {
        /* We want to start the session exactly at landmark position.
         * Run-times may have a non-[0,0,0] starting position that we have to subtract for that. */
        copy_v3_v3(draw_data->eye_position_ofs, draw_view->local_pose.position);
      }
      else {
        copy_v3_fl(draw_data->eye_position_ofs, 0.0f);
      }
      break;
      /* This should be triggered by the VR add-on if a landmark changes. */
    case SESSION_STATE_EVENT_RESET_TO_BASE_POSE:
      if (use_position_tracking) {
        /* Switch exactly to base pose, so use eye offset to cancel out current position delta. */
        copy_v3_v3(draw_data->eye_position_ofs, draw_view->local_pose.position);
      }
      else {
        copy_v3_fl(draw_data->eye_position_ofs, 0.0f);
      }
      break;
    case SESSION_STATE_EVENT_POSITION_TRACKING_TOGGLE:
      if (use_position_tracking) {
        /* Keep the current position, and let the user move from there. */
        copy_v3_v3(draw_data->eye_position_ofs, state->prev_eye_position_ofs);
      }
      else {
        /* Back to the exact base-pose position. */
        copy_v3_fl(draw_data->eye_position_ofs, 0.0f);
      }
      break;
    case SESSION_STATE_EVENT_NONE:
      /* Keep previous offset when positional tracking is disabled. */
      copy_v3_v3(draw_data->eye_position_ofs, state->prev_eye_position_ofs);
      break;
  }
}

/**
 * Update information that is only stored for external state queries. E.g. for Python API to
 * request the current (as in, last known) viewer pose.
 * Controller data and action sets will be updated separately via wm_xr_session_actions_update().
 */
void wm_xr_session_state_update(const XrSessionSettings *settings,
                                const wmXrDrawData *draw_data,
                                const GHOST_XrDrawViewInfo *draw_view,
                                wmXrSessionState *state)
{
  GHOST_XrPose viewer_pose;
  const bool use_position_tracking = settings->flag & XR_SESSION_USE_POSITION_TRACKING;
  const bool use_absolute_tracking = settings->flag & XR_SESSION_USE_ABSOLUTE_TRACKING;

  mul_qt_qtqt(viewer_pose.orientation_quat,
              draw_data->base_pose.orientation_quat,
              draw_view->local_pose.orientation_quat);
  copy_v3_v3(viewer_pose.position, draw_data->base_pose.position);
  /* The local pose and the eye pose (which is copied from an earlier local pose) both are view
   * space, so Y-up. In this case we need them in regular Z-up. */
  if (use_position_tracking) {
    viewer_pose.position[0] += draw_view->local_pose.position[0];
    viewer_pose.position[1] -= draw_view->local_pose.position[2];
    viewer_pose.position[2] += draw_view->local_pose.position[1];
  }
  if (!use_absolute_tracking) {
    viewer_pose.position[0] -= draw_data->eye_position_ofs[0];
    viewer_pose.position[1] += draw_data->eye_position_ofs[2];
    viewer_pose.position[2] -= draw_data->eye_position_ofs[1];
  }

  copy_v3_v3(state->viewer_pose.position, viewer_pose.position);
  copy_qt_qt(state->viewer_pose.orientation_quat, viewer_pose.orientation_quat);
  wm_xr_pose_to_imat(&viewer_pose, state->viewer_viewmat);
  /* No idea why, but multiplying by two seems to make it match the VR view more. */
  state->focal_len = 2.0f *
                     fov_to_focallength(draw_view->fov.angle_right - draw_view->fov.angle_left,
                                        DEFAULT_SENSOR_WIDTH);

  copy_v3_v3(state->prev_eye_position_ofs, draw_data->eye_position_ofs);
  memcpy(&state->prev_base_pose, &draw_data->base_pose, sizeof(state->prev_base_pose));
  memcpy(&state->prev_local_pose, &draw_view->local_pose, sizeof(state->prev_local_pose));
  state->prev_settings_flag = settings->flag;
  state->prev_base_pose_type = settings->base_pose_type;
  state->prev_base_pose_object = settings->base_pose_object;
  state->is_view_data_set = true;
  /* Assume this was already done through wm_xr_session_draw_data_update(). */
  state->force_reset_to_base_pose = false;
}

wmXrSessionState *WM_xr_session_state_handle_get(const wmXrData *xr)
{
  return xr->runtime ? &xr->runtime->session_state : NULL;
}

ScrArea *WM_xr_session_area_get(const wmXrData *xr)
{
  return xr->runtime ? xr->runtime->area : NULL;
}

bool WM_xr_session_state_viewer_pose_location_get(const wmXrData *xr, float r_location[3])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set) {
    zero_v3(r_location);
    return false;
  }

  copy_v3_v3(r_location, xr->runtime->session_state.viewer_pose.position);
  return true;
}

bool WM_xr_session_state_viewer_pose_rotation_get(const wmXrData *xr, float r_rotation[4])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set) {
    unit_qt(r_rotation);
    return false;
  }

  copy_v4_v4(r_rotation, xr->runtime->session_state.viewer_pose.orientation_quat);
  return true;
}

bool WM_xr_session_state_viewer_pose_matrix_info_get(const wmXrData *xr,
                                                     float r_viewmat[4][4],
                                                     float *r_focal_len)
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set) {
    unit_m4(r_viewmat);
    *r_focal_len = 0.0f;
    return false;
  }

  copy_m4_m4(r_viewmat, xr->runtime->session_state.viewer_viewmat);
  *r_focal_len = xr->runtime->session_state.focal_len;

  return true;
}

bool WM_xr_session_state_controller_grip_location_get(const wmXrData *xr,
                                                      unsigned int subaction_idx,
                                                      float r_location[3])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set ||
      (subaction_idx >= BLI_listbase_count(&xr->runtime->session_state.controllers))) {
    zero_v3(r_location);
    return false;
  }

  const wmXrController *controller = BLI_findlink(&xr->runtime->session_state.controllers,
                                                  subaction_idx);
  BLI_assert(controller);
  copy_v3_v3(r_location, controller->grip_pose.position);
  return true;
}

bool WM_xr_session_state_controller_grip_rotation_get(const wmXrData *xr,
                                                      unsigned int subaction_idx,
                                                      float r_rotation[4])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set ||
      (subaction_idx >= BLI_listbase_count(&xr->runtime->session_state.controllers))) {
    unit_qt(r_rotation);
    return false;
  }

  const wmXrController *controller = BLI_findlink(&xr->runtime->session_state.controllers,
                                                  subaction_idx);
  BLI_assert(controller);
  copy_qt_qt(r_rotation, controller->grip_pose.orientation_quat);
  return true;
}

bool WM_xr_session_state_controller_aim_location_get(const wmXrData *xr,
                                                     unsigned int subaction_idx,
                                                     float r_location[3])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set ||
      (subaction_idx >= BLI_listbase_count(&xr->runtime->session_state.controllers))) {
    zero_v3(r_location);
    return false;
  }

  const wmXrController *controller = BLI_findlink(&xr->runtime->session_state.controllers,
                                                  subaction_idx);
  BLI_assert(controller);
  copy_v3_v3(r_location, controller->aim_pose.position);
  return true;
}

bool WM_xr_session_state_controller_aim_rotation_get(const wmXrData *xr,
                                                     unsigned int subaction_idx,
                                                     float r_rotation[4])
{
  if (!WM_xr_session_is_ready(xr) || !xr->runtime->session_state.is_view_data_set ||
      (subaction_idx >= BLI_listbase_count(&xr->runtime->session_state.controllers))) {
    unit_qt(r_rotation);
    return false;
  }

  const wmXrController *controller = BLI_findlink(&xr->runtime->session_state.controllers,
                                                  subaction_idx);
  BLI_assert(controller);
  copy_qt_qt(r_rotation, controller->aim_pose.orientation_quat);
  return true;
}

/* -------------------------------------------------------------------- */
/** \name XR-Session Actions
 *
 * XR action processing and event dispatching.
 *
 * \{ */

void wm_xr_session_actions_init(wmXrData *xr)
{
  if (!xr->runtime) {
    return;
  }

  GHOST_XrAttachActionSets(xr->runtime->context);
}

static void wm_xr_session_controller_pose_calc(const GHOST_XrPose *raw_pose,
                                               const float view_ofs[3],
                                               const float base_mat[4][4],
                                               GHOST_XrPose *r_pose,
                                               float r_mat[4][4])
{
  float m[4][4];
  /* Calculate controller matrix in world space. */
  wm_xr_pose_to_mat(raw_pose, m);

  /* Apply eye position and base pose offsets. */
  sub_v3_v3(m[3], view_ofs);
  mul_m4_m4m4(r_mat, base_mat, m);

  /* Save final pose. */
  mat4_to_loc_quat(r_pose->position, r_pose->orientation_quat, r_mat);
}

static void wm_xr_session_controller_data_update(const XrSessionSettings *settings,
                                                 const wmXrAction *grip_action,
                                                 const wmXrAction *aim_action,
                                                 wmXrSessionState *state)
{
  BLI_assert(grip_action->count_subaction_paths == aim_action->count_subaction_paths);
  BLI_assert(grip_action->count_subaction_paths == BLI_listbase_count(&state->controllers));

  unsigned int subaction_idx = 0;
  float view_ofs[3], base_mat[4][4];

  if ((settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) {
    copy_v3_v3(view_ofs, state->prev_local_pose.position);
  }
  else {
    zero_v3(view_ofs);
  }
  if ((settings->flag & XR_SESSION_USE_ABSOLUTE_TRACKING) == 0) {
    add_v3_v3(view_ofs, state->prev_eye_position_ofs);
  }

  wm_xr_pose_to_mat(&state->prev_base_pose, base_mat);

  LISTBASE_FOREACH_INDEX (wmXrController *, controller, &state->controllers, subaction_idx) {
    wm_xr_session_controller_pose_calc(&((GHOST_XrPose *)grip_action->states)[subaction_idx],
                                       view_ofs,
                                       base_mat,
                                       &controller->grip_pose,
                                       controller->grip_mat);
    wm_xr_session_controller_pose_calc(&((GHOST_XrPose *)aim_action->states)[subaction_idx],
                                       view_ofs,
                                       base_mat,
                                       &controller->aim_pose,
                                       controller->aim_mat);
  }
}

void wm_xr_session_actions_update(wmWindowManager *wm)
{
  wmXrData *xr = &wm->xr;
  if (!xr->runtime) {
    return;
  }

  GHOST_XrContextHandle xr_context = xr->runtime->context;
  wmXrSessionState *state = &xr->runtime->session_state;
  wmXrActionSet *active_action_set = state->active_action_set;

  int ret = GHOST_XrSyncActions(xr_context, active_action_set ? active_action_set->name : NULL);
  if (!ret) {
    return;
  }

  /* Only update controller data and dispatch events for active action set. */
  if (active_action_set) {
    const XrSessionSettings *settings = &xr->session_settings;
    wmWindow *win = wm_xr_session_root_window_or_fallback_get(wm, xr->runtime);

    if (active_action_set->controller_grip_action && active_action_set->controller_aim_action) {
      wm_xr_session_controller_data_update(settings,
                                           active_action_set->controller_grip_action,
                                           active_action_set->controller_aim_action,
                                           state);
    }

    if (win) {
      /* Ensure an XR area exists for events. */
      if (!xr->runtime->area) {
        xr->runtime->area = ED_area_offscreen_create(win, SPACE_VIEW3D);
      }

      /* Implemented in D10944. */
      // wm_xr_session_events_dispatch(xr, settings, xr_context, active_action_set, state, win);
    }
  }
}

void wm_xr_session_controller_data_populate(const wmXrAction *grip_action,
                                            const wmXrAction *aim_action,
                                            wmXrData *xr)
{
  UNUSED_VARS(aim_action); /* Only used for asserts. */

  wmXrSessionState *state = &xr->runtime->session_state;
  ListBase *controllers = &state->controllers;

  BLI_assert(grip_action->count_subaction_paths == aim_action->count_subaction_paths);
  const unsigned int count = grip_action->count_subaction_paths;

  wm_xr_session_controller_data_free(state);

  for (unsigned int i = 0; i < count; ++i) {
    wmXrController *controller = MEM_callocN(sizeof(*controller), __func__);

    BLI_assert(STREQ(grip_action->subaction_paths[i], aim_action->subaction_paths[i]));
    strcpy(controller->subaction_path, grip_action->subaction_paths[i]);

    BLI_addtail(controllers, controller);
  }
}

void wm_xr_session_controller_data_clear(wmXrSessionState *state)
{
  wm_xr_session_controller_data_free(state);
}

/** \} */ /* XR-Session Actions */

/* -------------------------------------------------------------------- */
/** \name XR-Session Surface
 *
 * A wmSurface is used to manage drawing of the VR viewport. It's created and destroyed with the
 * session.
 *
 * \{ */

/**
 * \brief Call Ghost-XR to draw a frame
 *
 * Draw callback for the XR-session surface. It's expected to be called on each main loop
 * iteration and tells Ghost-XR to submit a new frame by drawing its views. Note that for drawing
 * each view, #wm_xr_draw_view() will be called through Ghost-XR (see GHOST_XrDrawViewFunc()).
 */
static void wm_xr_session_surface_draw(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  Main *bmain = CTX_data_main(C);
  wmXrDrawData draw_data;

  if (!WM_xr_session_is_ready(&wm->xr)) {
    return;
  }

  Scene *scene;
  Depsgraph *depsgraph;
  wm_xr_session_scene_and_evaluated_depsgraph_get(bmain, wm, &scene, &depsgraph);
  wm_xr_session_draw_data_populate(&wm->xr, scene, depsgraph, &draw_data);

  GHOST_XrSessionDrawViews(wm->xr.runtime->context, &draw_data);

  GPU_framebuffer_restore();
}

bool wm_xr_session_surface_offscreen_ensure(wmXrSurfaceData *surface_data,
                                            const GHOST_XrDrawViewInfo *draw_view)
{
  wmXrViewportPair *vp = NULL;
  if (draw_view->view_idx >= BLI_listbase_count(&surface_data->viewports)) {
    vp = MEM_callocN(sizeof(*vp), __func__);
    BLI_addtail(&surface_data->viewports, vp);
  }
  else {
    vp = BLI_findlink(&surface_data->viewports, draw_view->view_idx);
  }
  BLI_assert(vp);

  GPUOffScreen *offscreen = vp->offscreen;
  GPUViewport *viewport = vp->viewport;
  const bool size_changed = offscreen && (GPU_offscreen_width(offscreen) != draw_view->width) &&
                            (GPU_offscreen_height(offscreen) != draw_view->height);
  if (offscreen) {
    BLI_assert(viewport);

    if (!size_changed) {
      return true;
    }
    GPU_viewport_free(viewport);
    GPU_offscreen_free(offscreen);
  }

  char err_out[256] = "unknown";
  bool failure = false;
  eGPUTextureFormat format =
      GPU_R8; /* Initialize with some unsupported format to check following switch statement. */

  switch (draw_view->swapchain_format) {
    case GHOST_kXrSwapchainFormatRGBA8:
      format = GPU_RGBA8;
      break;
    case GHOST_kXrSwapchainFormatRGBA16:
      format = GPU_RGBA16;
      break;
    case GHOST_kXrSwapchainFormatRGBA16F:
      format = GPU_RGBA16F;
      break;
    case GHOST_kXrSwapchainFormatRGB10_A2:
      format = GPU_RGB10_A2;
      break;
  }
  BLI_assert(format != GPU_R8);

  offscreen = vp->offscreen = GPU_offscreen_create(
      draw_view->width, draw_view->height, true, format, err_out);
  if (offscreen) {
    viewport = vp->viewport = GPU_viewport_create();
    if (!viewport) {
      GPU_offscreen_free(offscreen);
      offscreen = vp->offscreen = NULL;
      failure = true;
    }
  }
  else {
    failure = true;
  }

  if (failure) {
    CLOG_ERROR(&LOG, "Failed to get buffer, %s", err_out);
    return false;
  }

  return true;
}

static void wm_xr_session_surface_free_data(wmSurface *surface)
{
  wmXrSurfaceData *data = surface->customdata;
  ListBase *lb = &data->viewports;
  wmXrViewportPair *vp;

  while ((vp = BLI_pophead(lb))) {
    if (vp->viewport) {
      GPU_viewport_free(vp->viewport);
    }
    if (vp->offscreen) {
      GPU_offscreen_free(vp->offscreen);
    }
    BLI_freelinkN(lb, vp);
  }

  MEM_freeN(surface->customdata);

  g_xr_surface = NULL;
}

static wmSurface *wm_xr_session_surface_create(void)
{
  if (g_xr_surface) {
    BLI_assert(false);
    return g_xr_surface;
  }

  wmSurface *surface = MEM_callocN(sizeof(*surface), __func__);
  wmXrSurfaceData *data = MEM_callocN(sizeof(*data), "XrSurfaceData");

  surface->draw = wm_xr_session_surface_draw;
  surface->free_data = wm_xr_session_surface_free_data;
  surface->activate = DRW_xr_drawing_begin;
  surface->deactivate = DRW_xr_drawing_end;

  surface->ghost_ctx = DRW_xr_opengl_context_get();
  surface->gpu_ctx = DRW_xr_gpu_context_get();

  surface->customdata = data;

  g_xr_surface = surface;

  return surface;
}

void *wm_xr_session_gpu_binding_context_create(void)
{
  wmSurface *surface = wm_xr_session_surface_create();

  wm_surface_add(surface);

  /* Some regions may need to redraw with updated session state after the session is entirely up
   * and running. */
  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);

  return surface->ghost_ctx;
}

void wm_xr_session_gpu_binding_context_destroy(GHOST_ContextHandle UNUSED(context))
{
  if (g_xr_surface) { /* Might have been freed already */
    wm_surface_remove(g_xr_surface);
  }

  wm_window_reset_drawable();

  /* Some regions may need to redraw with updated session state after the session is entirely
   * stopped. */
  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);
}

/** \} */ /* XR-Session Surface */
