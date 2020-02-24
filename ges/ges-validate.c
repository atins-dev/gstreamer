/* GStreamer Editing Services
 *
 * Copyright (C) <2014> Thibault Saunier <thibault.saunier@collabora.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ges/ges.h>

#ifdef HAVE_GST_VALIDATE
#include <gst/validate/validate.h>
#include <gst/validate/gst-validate-scenario.h>
#include <gst/validate/gst-validate-utils.h>
#include "ges-internal.h"
#include "ges-structured-interface.h"

#define MONITOR_ON_PIPELINE "validate-monitor"
#define RUNNER_ON_PIPELINE "runner-monitor"

typedef struct
{
  GMainLoop *ml;
  GError *error;
} LoadTimelineData;

static void
project_loaded_cb (GESProject * project, GESTimeline * timeline,
    LoadTimelineData * data)
{
  g_main_loop_quit (data->ml);
}

static void
error_loading_asset_cb (GESProject * project, GError * err,
    const gchar * unused_id, GType extractable_type, LoadTimelineData * data)
{
  data->error = g_error_copy (err);
  g_main_loop_quit (data->ml);
}

static GESTimeline *
_ges_load_timeline (GstValidateScenario * scenario, GstValidateAction * action,
    const gchar * project_uri)
{
  GESProject *project = ges_project_new (project_uri);
  GESTimeline *timeline;
  LoadTimelineData data = { 0 };

  data.ml = g_main_loop_new (NULL, TRUE);
  timeline =
      GES_TIMELINE (ges_asset_extract (GES_ASSET (project), &data.error));
  if (!timeline)
    goto done;

  g_signal_connect (project, "loaded", (GCallback) project_loaded_cb, &data);
  g_signal_connect (project, "error-loading-asset",
      (GCallback) error_loading_asset_cb, &data);
  g_main_loop_run (data.ml);
  g_signal_handlers_disconnect_by_func (project, project_loaded_cb, &data);
  g_signal_handlers_disconnect_by_func (project, error_loading_asset_cb, &data);
  GST_INFO_OBJECT (scenario, "Loaded timeline from %s", project_uri);

done:
  if (data.error) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        SCENARIO_ACTION_EXECUTION_ERROR, "Can not load timeline from: %s (%s)",
        project_uri, data.error->message);
    g_clear_error (&data.error);
    gst_clear_object (&timeline);
  }

  g_main_loop_unref (data.ml);
  gst_object_unref (project);
  return timeline;
}

#define DECLARE_AND_GET_TIMELINE_AND_PIPELINE(scenario, action)                                   \
    GESTimeline* timeline;                                                                        \
    GstElement* pipeline = NULL;                                                                  \
    const gchar* project_uri = gst_structure_get_string(action->structure, "project-uri"); \
    if (!project_uri) {                                                                    \
        pipeline = gst_validate_scenario_get_pipeline(scenario);                                  \
        if (pipeline == NULL) {                                                                   \
            GST_VALIDATE_REPORT_ACTION (scenario, action, SCENARIO_ACTION_EXECUTION_ERROR,        \
                "Can't execute a '%s' action after the pipeline "                                 \
                "has been destroyed.",                                                            \
                action->type);                                                                    \
            return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;                                    \
        }                                                                                         \
        g_object_get(pipeline, "timeline", &timeline, NULL);                                      \
    } else {                                                                                      \
        timeline = _ges_load_timeline(scenario, action, project_uri);                             \
        if (!timeline)                                                                            \
            return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;                                    \
    }

#define DECLARE_AND_GET_TIMELINE(scenario, action)         \
  DECLARE_AND_GET_TIMELINE_AND_PIPELINE(scenario, action); \
  if (pipeline)                                            \
      gst_object_unref(pipeline);                          \

#define SAVE_TIMELINE_IF_NEEDED(scenario, timeline, action)                                                  \
    {                                                                                                        \
        if (!_ges_save_timeline_if_needed(timeline, action->structure, NULL)) {                              \
            GST_VALIDATE_REPORT_ACTION(scenario, action,                                                     \
                g_quark_from_string("scenario::execution-error"),                                            \
                "Could not save timeline to %s", gst_structure_get_string(action->structure, "project-id")); \
            gst_object_unref(timeline);                                                                      \
            return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;                                               \
        }                                                                                                    \
    }

#define TRY_GET(name,type,var,def) G_STMT_START {\
  if  (!gst_structure_get (action->structure, name, type, var, NULL)) {\
    *var = def; \
  } \
} G_STMT_END

static gboolean
_serialize_project (GstValidateScenario * scenario, GstValidateAction * action)
{
  const gchar *uri = gst_structure_get_string (action->structure, "uri");
  gchar *location = gst_uri_get_location (uri),
      *dir = g_path_get_dirname (location);
  gboolean res;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  gst_validate_printf (action, "Saving project to %s", uri);

  g_mkdir_with_parents (dir, 0755);
  g_free (location);
  g_free (dir);

  res = ges_timeline_save_to_uri (timeline, uri, NULL, TRUE, NULL);

  g_object_unref (timeline);
  return res;
}

static gboolean
_remove_asset (GstValidateScenario * scenario, GstValidateAction * action)
{
  const gchar *id = NULL;
  const gchar *type_string = NULL;
  GType type;
  GESAsset *asset;
  gboolean res = FALSE;
  GESProject *project;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  project = ges_timeline_get_project (timeline);

  id = gst_structure_get_string (action->structure, "id");
  type_string = gst_structure_get_string (action->structure, "type");

  if (!type_string || !id) {
    GST_ERROR ("Missing parameters, we got type %s and id %s", type_string, id);
    goto beach;
  }

  if (!(type = g_type_from_name (type_string))) {
    GST_ERROR ("This type doesn't exist : %s", type_string);
    goto beach;
  }

  asset = ges_project_get_asset (project, id, type);

  if (!asset) {
    GST_ERROR ("No asset with id %s and type %s", id, type_string);
    goto beach;
  }

  res = ges_project_remove_asset (project, asset);
  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
beach:
  g_object_unref (timeline);
  return res;
}

static gboolean
_add_asset (GstValidateScenario * scenario, GstValidateAction * action)
{
  const gchar *id = NULL;
  const gchar *type_string = NULL;
  GType type;
  GESAsset *asset;
  gboolean res = FALSE;
  GESProject *project;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  project = ges_timeline_get_project (timeline);

  id = gst_structure_get_string (action->structure, "id");
  type_string = gst_structure_get_string (action->structure, "type");

  gst_validate_printf (action, "Adding asset of type %s with ID %s\n",
      id, type_string);

  if (!type_string || !id) {
    GST_ERROR ("Missing parameters, we got type %s and id %s", type_string, id);
    goto beach;
  }

  if (!(type = g_type_from_name (type_string))) {
    GST_ERROR ("This type doesn't exist : %s", type_string);
    goto beach;
  }

  asset = _ges_get_asset_from_timeline (timeline, type, id, NULL);

  if (!asset) {
    res = FALSE;

    goto beach;
  }

  res = ges_project_add_asset (project, asset);
  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);

beach:
  g_object_unref (timeline);
  return res;
}

static gboolean
_add_layer (GstValidateScenario * scenario, GstValidateAction * action)
{
  GESLayer *layer;
  gint priority;
  gboolean res = TRUE, auto_transition = FALSE;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  if (!gst_structure_get_int (action->structure, "priority", &priority)) {
    GST_ERROR ("priority is needed when adding a layer");
    goto failed;
  }

  gst_validate_printf (action, "Adding layer with priority %d\n", priority);
  layer = _ges_get_layer_by_priority (timeline, priority);

  gst_structure_get_boolean (action->structure, "auto-transition",
      &auto_transition);
  g_object_set (layer, "priority", priority, "auto-transition", auto_transition,
      NULL);


  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
beach:
  g_object_unref (timeline);
  return res;

failed:
  goto beach;
}

static gboolean
_remove_layer (GstValidateScenario * scenario, GstValidateAction * action)
{
  GESLayer *layer;
  gint priority;
  gboolean res = FALSE;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  if (!gst_structure_get_int (action->structure, "priority", &priority)) {
    GST_ERROR ("priority is needed when removing a layer");
    goto beach;
  }

  layer = _ges_get_layer_by_priority (timeline, priority);

  if (layer) {
    res = ges_timeline_remove_layer (timeline, layer);
    gst_object_unref (layer);
  } else {
    GST_ERROR ("No layer with priority %d", priority);
  }

  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
beach:
  g_object_unref (timeline);
  return res;
}

static gboolean
_remove_clip (GstValidateScenario * scenario, GstValidateAction * action)
{
  GESTimelineElement *clip;
  GESLayer *layer;
  const gchar *name;
  gboolean res = FALSE;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  name = gst_structure_get_string (action->structure, "name");
  clip = ges_timeline_get_element (timeline, name);
  g_return_val_if_fail (GES_IS_CLIP (clip), FALSE);

  gst_validate_printf (action, "removing clip with ID %s\n", name);
  layer = ges_clip_get_layer (GES_CLIP (clip));

  if (layer) {
    res = ges_layer_remove_clip (layer, GES_CLIP (clip));
    gst_object_unref (layer);
  } else {
    GST_ERROR ("No layer for clip %s", ges_timeline_element_get_name (clip));
  }

  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
  g_object_unref (timeline);
  return res;
}


static gboolean
_edit_container (GstValidateScenario * scenario, GstValidateAction * action)
{
  GList *layers = NULL;
  GESTimelineElement *container;
  GstClockTime position;
  gboolean res = FALSE;

  gint new_layer_priority = -1;
  guint edge = GES_EDGE_NONE;
  guint mode = GES_EDIT_MODE_NORMAL;

  const gchar *edit_mode_str = NULL, *edge_str = NULL;
  const gchar *clip_name;

  DECLARE_AND_GET_TIMELINE (scenario, action);

  clip_name = gst_structure_get_string (action->structure, "container-name");

  container = ges_timeline_get_element (timeline, clip_name);
  if (!container) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        SCENARIO_ACTION_EXECUTION_ERROR,
        "Could not find container %s", clip_name);
    return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
  }

  if (!gst_validate_action_get_clocktime (scenario, action,
          "position", &position)) {
    GST_WARNING ("Could not get position");
    goto beach;
  }

  if ((edit_mode_str =
          gst_structure_get_string (action->structure, "edit-mode")))
    g_return_val_if_fail (gst_validate_utils_enum_from_str (GES_TYPE_EDIT_MODE,
            edit_mode_str, &mode), FALSE);

  if ((edge_str = gst_structure_get_string (action->structure, "edge")))
    g_return_val_if_fail (gst_validate_utils_enum_from_str (GES_TYPE_EDGE,
            edge_str, &edge), FALSE);

  gst_structure_get_int (action->structure, "new-layer-priority",
      &new_layer_priority);

  gst_validate_printf (action, "Editing %s to %" GST_TIME_FORMAT
      " in %s mode, edge: %s "
      "with new layer prio: %d \n\n",
      clip_name, GST_TIME_ARGS (position),
      edit_mode_str ? edit_mode_str : "normal",
      edge_str ? edge_str : "None", new_layer_priority);

  if (!(res = ges_container_edit (GES_CONTAINER (container), layers,
              new_layer_priority, mode, edge, position))) {
    gst_object_unref (container);
    GST_ERROR ("HERE");
    goto beach;
  }
  gst_object_unref (container);

  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
beach:
  g_object_unref (timeline);
  return res;
}


static void
_commit_done_cb (GstBus * bus, GstMessage * message, GstValidateAction * action)
{
  gst_validate_action_set_done (action);

  g_signal_handlers_disconnect_by_func (bus, _commit_done_cb, action);
}

static gboolean
_commit (GstValidateScenario * scenario, GstValidateAction * action)
{
  GstBus *bus;
  GstState state;

  DECLARE_AND_GET_TIMELINE_AND_PIPELINE (scenario, action);

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));

  gst_validate_printf (action, "Commiting timeline %s\n",
      GST_OBJECT_NAME (timeline));

  g_signal_connect (bus, "message::async-done", G_CALLBACK (_commit_done_cb),
      action);

  gst_element_get_state (pipeline, &state, NULL, 0);
  if (!ges_timeline_commit (timeline) || state < GST_STATE_PAUSED) {
    g_signal_handlers_disconnect_by_func (bus, G_CALLBACK (_commit_done_cb),
        action);
    gst_object_unref (timeline);
    gst_object_unref (bus);

    return TRUE;
  }
  gst_object_unref (bus);
  gst_object_unref (timeline);
  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);

  return GST_VALIDATE_EXECUTE_ACTION_ASYNC;
}

static gboolean
_split_clip (GstValidateScenario * scenario, GstValidateAction * action)
{
  const gchar *clip_name;
  GESTimelineElement *element;
  GstClockTime position;

  DECLARE_AND_GET_TIMELINE (scenario, action);

  clip_name = gst_structure_get_string (action->structure, "clip-name");

  element = ges_timeline_get_element (timeline, clip_name);
  g_return_val_if_fail (GES_IS_CLIP (element), FALSE);
  g_object_unref (timeline);

  g_return_val_if_fail (gst_validate_action_get_clocktime (scenario, action,
          "position", &position), FALSE);

  return (ges_clip_split (GES_CLIP (element), position) != NULL);
}

typedef struct
{
  GstValidateScenario *scenario;
  GESTimelineElement *element;
  GstValidateActionReturn res;
  GstClockTime time;
  gboolean check_children;
  GstValidateAction *action;
} PropertyData;

static gboolean
check_property (GQuark field_id, GValue * expected_value, PropertyData * data)
{
  GValue cvalue = G_VALUE_INIT, *tvalue = NULL, comparable_value = G_VALUE_INIT,
      *observed_value;
  const gchar *property = g_quark_to_string (field_id);
  GstControlBinding *binding = NULL;

  if (!data->check_children) {
    g_object_get_property (G_OBJECT (data->element), property, &cvalue);
    goto compare;
  }

  if (GST_CLOCK_TIME_IS_VALID (data->time)) {
    if (!GES_IS_TRACK_ELEMENT (data->element)) {
      GST_VALIDATE_REPORT_ACTION (data->scenario, data->action,
          SCENARIO_ACTION_EXECUTION_ERROR,
          "Could not get property at time for type %s - only GESTrackElement supported",
          G_OBJECT_TYPE_NAME (data->element));
      data->res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;

      return FALSE;
    }

    binding =
        ges_track_element_get_control_binding (GES_TRACK_ELEMENT
        (data->element), property);
    if (binding) {
      tvalue = gst_control_binding_get_value (binding, data->time);

      if (!tvalue) {
        GST_VALIDATE_REPORT_ACTION (data->scenario, data->action,
            SCENARIO_ACTION_EXECUTION_ERROR,
            "Could not get property: %s at %" GST_TIME_FORMAT, property,
            GST_TIME_ARGS (data->time));
        data->res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;

        return FALSE;
      }
    }
  }

  if (!tvalue
      && !ges_timeline_element_get_child_property (data->element, property,
          &cvalue)) {
    GST_VALIDATE_REPORT_ACTION (data->scenario, data->action,
        SCENARIO_ACTION_EXECUTION_ERROR, "Could not get property: %s:",
        property);
    data->res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;

    return FALSE;
  }

compare:
  observed_value = tvalue ? tvalue : &cvalue;

  if (G_VALUE_TYPE (observed_value) != G_VALUE_TYPE (expected_value)) {
    g_value_init (&comparable_value, G_VALUE_TYPE (observed_value));

    if (G_VALUE_TYPE (observed_value) == GST_TYPE_CLOCK_TIME) {
      GstClockTime t;

      if (gst_validate_utils_get_clocktime (data->action->structure, property,
              &t)) {
        g_value_set_uint64 (&comparable_value, t);
        expected_value = &comparable_value;
      }
    } else if (g_value_transform (expected_value, &comparable_value)) {
      expected_value = &comparable_value;
    }
  }

  if (gst_value_compare (observed_value, expected_value) != GST_VALUE_EQUAL) {
    gchar *expected = gst_value_serialize (expected_value), *observed =
        gst_value_serialize (observed_value);

    GST_VALIDATE_REPORT_ACTION (data->scenario, data->action,
        SCENARIO_ACTION_CHECK_ERROR,
        "%s:%s expected value: '(%s)%s' different than observed: '(%s)%s'",
        GES_TIMELINE_ELEMENT_NAME (data->element), property,
        G_VALUE_TYPE_NAME (observed_value), expected,
        G_VALUE_TYPE_NAME (expected_value), observed);

    g_free (expected);
    g_free (observed);
    data->res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
  }

  if (G_VALUE_TYPE (&comparable_value) != G_TYPE_NONE)
    g_value_unset (&comparable_value);

  if (tvalue) {
    g_value_unset (tvalue);
    g_free (tvalue);
  } else
    g_value_reset (&cvalue);
  return TRUE;
}

static gboolean
set_property (GQuark field_id, const GValue * value, PropertyData * data)
{
  const gchar *property = g_quark_to_string (field_id);

  if (!ges_timeline_element_set_child_property (data->element, property, value)) {
    gchar *v = gst_value_serialize (value);

    GST_VALIDATE_REPORT_ACTION (data->scenario, data->action,
        SCENARIO_ACTION_EXECUTION_ERROR,
        "Could not set %s child property %s to %s",
        GES_TIMELINE_ELEMENT_NAME (data->element), property, v);

    data->res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
    g_free (v);

    return FALSE;
  }

  return TRUE;
}

static gboolean
set_or_check_properties (GstValidateScenario * scenario,
    GstValidateAction * action)
{
  GESTimelineElement *element;
  GstStructure *structure;
  const gchar *element_name;
  PropertyData data = {
    .scenario = scenario,
    .element = NULL,
    .res = GST_VALIDATE_EXECUTE_ACTION_OK,
    .time = GST_CLOCK_TIME_NONE,
    .check_children =
        !gst_structure_has_name (action->structure, "check-ges-properties"),
    .action = action,
  };

  DECLARE_AND_GET_TIMELINE (scenario, action);

  gst_validate_action_get_clocktime (scenario, action, "at-time", &data.time);

  structure = gst_structure_copy (action->structure);
  element_name = gst_structure_get_string (structure, "element-name");
  element = ges_timeline_get_element (timeline, element_name);
  if (!element) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        SCENARIO_ACTION_EXECUTION_ERROR,
        "Can not find element: %s", element_name);

    data.res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
    goto done;
  }
  g_object_unref (timeline);

  data.element = element;
  gst_structure_remove_fields (structure, "element-name", "at-time", NULL);
  gst_structure_foreach (structure,
      gst_structure_has_name (action->structure,
          "set-child-properties") ? (GstStructureForeachFunc) set_property
      : (GstStructureForeachFunc) check_property, &data);
  gst_object_unref (element);

done:
  gst_structure_free (structure);

  return data.res;
}

static gboolean
_set_track_restriction_caps (GstValidateScenario * scenario,
    GstValidateAction * action)
{
  GList *tmp;
  GstCaps *caps;
  gboolean res = FALSE;
  GESTrackType track_types;

  const gchar *track_type_str =
      gst_structure_get_string (action->structure, "track-type");
  const gchar *caps_str = gst_structure_get_string (action->structure, "caps");

  DECLARE_AND_GET_TIMELINE (scenario, action);

  g_return_val_if_fail ((track_types =
          gst_validate_utils_flags_from_str (GES_TYPE_TRACK_TYPE,
              track_type_str)), FALSE);

  g_return_val_if_fail ((caps =
          gst_caps_from_string (caps_str)) != NULL, FALSE);

  for (tmp = timeline->tracks; tmp; tmp = tmp->next) {
    GESTrack *track = tmp->data;

    if (track->type & track_types) {
      gchar *str;

      str = gst_caps_to_string (caps);
      gst_validate_printf (action, "Setting restriction caps %s on track: %s\n",
          str, GST_ELEMENT_NAME (track));
      g_free (str);

      ges_track_set_restriction_caps (track, caps);

      res = TRUE;
    }
  }
  gst_caps_unref (caps);
  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
  gst_object_unref (timeline);

  return res;
}

static gboolean
_set_asset_on_element (GstValidateScenario * scenario,
    GstValidateAction * action)
{
  GESAsset *asset;
  GESTimelineElement *element;
  const gchar *element_name, *id;

  gboolean res = TRUE;
  DECLARE_AND_GET_TIMELINE (scenario, action);

  element_name = gst_structure_get_string (action->structure, "element-name");
  element = ges_timeline_get_element (timeline, element_name);
  g_return_val_if_fail (GES_IS_TIMELINE_ELEMENT (element), FALSE);

  id = gst_structure_get_string (action->structure, "asset-id");

  gst_validate_printf (action, "Setting asset %s on element %s\n",
      id, element_name);

  asset = _ges_get_asset_from_timeline (timeline, G_OBJECT_TYPE (element), id,
      NULL);
  if (asset == NULL) {
    res = FALSE;
    GST_ERROR ("Could not find asset: %s", id);
    goto beach;
  }

  res = ges_extractable_set_asset (GES_EXTRACTABLE (element), asset);
  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);

beach:
  gst_object_unref (timeline);

  return res;
}

static gboolean
_container_remove_child (GstValidateScenario * scenario,
    GstValidateAction * action)
{
  GESContainer *container;
  GESTimelineElement *child;
  const gchar *container_name, *child_name;

  gboolean res = TRUE;

  DECLARE_AND_GET_TIMELINE (scenario, action);

  container_name =
      gst_structure_get_string (action->structure, "container-name");
  container =
      GES_CONTAINER (ges_timeline_get_element (timeline, container_name));
  g_return_val_if_fail (GES_IS_CONTAINER (container), FALSE);

  child_name = gst_structure_get_string (action->structure, "child-name");
  child = ges_timeline_get_element (timeline, child_name);
  g_return_val_if_fail (GES_IS_TIMELINE_ELEMENT (child), FALSE);

  gst_validate_printf (action, "Remove child %s from container %s\n",
      child_name, GES_TIMELINE_ELEMENT_NAME (container));

  res = ges_container_remove (container, child);

  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
  gst_object_unref (timeline);

  return res;
}

static gboolean
_ungroup (GstValidateScenario * scenario, GstValidateAction * action)
{
  GESContainer *container;
  gboolean recursive = FALSE;
  const gchar *container_name;

  gboolean res = TRUE;

  DECLARE_AND_GET_TIMELINE (scenario, action);

  container_name =
      gst_structure_get_string (action->structure, "container-name");
  container =
      GES_CONTAINER (ges_timeline_get_element (timeline, container_name));
  g_return_val_if_fail (GES_IS_CONTAINER (container), FALSE);

  gst_validate_printf (action, "Ungrouping children from container %s\n",
      GES_TIMELINE_ELEMENT_NAME (container));

  gst_structure_get_boolean (action->structure, "recursive", &recursive);

  g_list_free (ges_container_ungroup (container, recursive));

  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
  gst_object_unref (timeline);

  return res;
}

static GstValidateExecuteActionReturn
_copy_element (GstValidateScenario * scenario, GstValidateAction * action)
{
  GESTimelineElement *element, *copied, *pasted;
  gboolean recursive = FALSE;
  const gchar *element_name, *paste_name;
  GstClockTime position;
  DECLARE_AND_GET_TIMELINE (scenario, action);


  element_name = gst_structure_get_string (action->structure, "element-name");
  element = ges_timeline_get_element (timeline, element_name);

  g_return_val_if_fail (GES_IS_TIMELINE_ELEMENT (element),
      GST_VALIDATE_EXECUTE_ACTION_ERROR);

  gst_validate_printf (action, "Copying element %s\n",
      GES_TIMELINE_ELEMENT_NAME (element));

  if (!gst_structure_get_boolean (action->structure, "recursive", &recursive))
    recursive = TRUE;

  g_return_val_if_fail (gst_validate_action_get_clocktime (scenario, action,
          "position", &position), FALSE);

  copied = ges_timeline_element_copy (element, recursive);
  pasted = ges_timeline_element_paste (copied, position);
  gst_object_unref (timeline);

  if (!pasted) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        g_quark_from_string ("scenario::execution-error"),
        "Could not paste clip %s", element_name);
    return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
  }

  paste_name = gst_structure_get_string (action->structure, "paste-name");
  if (paste_name) {
    if (!ges_timeline_element_set_name (pasted, paste_name)) {
      GST_VALIDATE_REPORT_ACTION (scenario, action,
          g_quark_from_string ("scenario::execution-error"),
          "Could not set element name %s", paste_name);

      return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
    }
  }

  return GST_VALIDATE_EXECUTE_ACTION_OK;
}

static gboolean
_set_control_source (GstValidateScenario * scenario, GstValidateAction * action)
{
  GESTrackElement *element;

  gboolean ret = FALSE;
  GstControlSource *source = NULL;
  gchar *element_name, *property_name, *binding_type = NULL,
      *source_type = NULL, *interpolation_mode = NULL;

  DECLARE_AND_GET_TIMELINE (scenario, action);

  g_return_val_if_fail (gst_structure_get (action->structure,
          "element-name", G_TYPE_STRING, &element_name,
          "property-name", G_TYPE_STRING, &property_name, NULL), FALSE);

  TRY_GET ("binding-type", G_TYPE_STRING, &binding_type, NULL);
  TRY_GET ("source-type", G_TYPE_STRING, &source_type, NULL);
  TRY_GET ("interpolation-mode", G_TYPE_STRING, &interpolation_mode, NULL);

  element =
      GES_TRACK_ELEMENT (ges_timeline_get_element (timeline, element_name));
  g_return_val_if_fail (GES_IS_TRACK_ELEMENT (element), FALSE);

  if (!binding_type)
    binding_type = g_strdup ("direct");

  if (source_type == NULL || !g_strcmp0 (source_type, "interpolation")) {
    guint mode;

    source = gst_interpolation_control_source_new ();

    if (interpolation_mode)
      g_return_val_if_fail (gst_validate_utils_enum_from_str
          (GST_TYPE_INTERPOLATION_MODE, interpolation_mode, &mode), FALSE);
    else
      mode = GST_INTERPOLATION_MODE_LINEAR;

    g_object_set (source, "mode", mode, NULL);

  } else {
    GST_ERROR_OBJECT (scenario, "Interpolation type %s not supported",
        source_type);

    goto done;
  }

  gst_validate_printf (action, "Setting control source on %s:%s\n",
      element_name, property_name);
  ret = ges_track_element_set_control_source (element,
      source, property_name, binding_type);

done:
  g_free (property_name);
  g_free (element_name);
  g_free (binding_type);
  g_free (source_type);
  g_free (interpolation_mode);

  SAVE_TIMELINE_IF_NEEDED (scenario, timeline, action);
  gst_object_unref (timeline);

  return ret;
}

static gboolean
_validate_action_execute (GstValidateScenario * scenario,
    GstValidateAction * action)
{
  GError *err = NULL;
  ActionFromStructureFunc func;

  DECLARE_AND_GET_TIMELINE (scenario, action);

  gst_structure_remove_field (action->structure, "playback-time");
  if (gst_structure_has_name (action->structure, "add-keyframe") ||
      gst_structure_has_name (action->structure, "remove-keyframe")) {
    func = _ges_add_remove_keyframe_from_struct;
  } else if (gst_structure_has_name (action->structure, "add-clip")) {
    func = _ges_add_clip_from_struct;
  } else if (gst_structure_has_name (action->structure, "container-add-child")) {
    func = _ges_container_add_child_from_struct;
  } else if (gst_structure_has_name (action->structure, "set-child-property")) {
    func = _ges_set_child_property_from_struct;
  } else {
    g_assert_not_reached ();
  }

  if (!func (timeline, action->structure, &err)) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        g_quark_from_string ("scenario::execution-error"),
        "Could not execute %s (error: %s)",
        gst_structure_get_name (action->structure),
        err ? err->message : "None");

    g_clear_error (&err);

    return TRUE;
  }

  gst_object_unref (timeline);

  return TRUE;
}

static void
_project_loaded_cb (GESProject * project, GESTimeline * timeline,
    GstValidateAction * action)
{
  gst_validate_action_set_done (action);
}

static gboolean
_load_project (GstValidateScenario * scenario, GstValidateAction * action)
{
  GstState state;
  GESProject *project;
  GList *tmp, *tmp_full;

  gchar *uri = NULL;
  GError *error = NULL;
  const gchar *content = NULL;

  gchar *tmpfile = g_strdup_printf ("%s%s%s", g_get_tmp_dir (),
      G_DIR_SEPARATOR_S, "tmpxgesload.xges");

  GstValidateExecuteActionReturn res = GST_VALIDATE_EXECUTE_ACTION_ASYNC;
  DECLARE_AND_GET_TIMELINE_AND_PIPELINE (scenario, action);

  gst_validate_printf (action, "Loading project from serialized content\n");

  if (!GES_IS_PIPELINE (pipeline)) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        g_quark_from_string ("scenario::execution-error"),
        "Not a GES pipeline, can't work with it");

    goto fail;
  }
  gst_element_get_state (pipeline, &state, NULL, 0);
  gst_element_set_state (pipeline, GST_STATE_NULL);

  content = gst_structure_get_string (action->structure, "serialized-content");
  if (content) {

    g_file_set_contents (tmpfile, content, -1, &error);
    if (error) {
      GST_VALIDATE_REPORT_ACTION (scenario, action,
          g_quark_from_string ("scenario::execution-error"),
          "Could not set XML content: %s", error->message);

      goto fail;
    }

    uri = gst_filename_to_uri (tmpfile, &error);
    if (error) {
      GST_VALIDATE_REPORT_ACTION (scenario, action,
          g_quark_from_string ("scenario::execution-error"),
          "Could not set filename to URI: %s", error->message);

      goto fail;
    }
  } else {
    uri = g_strdup (gst_structure_get_string (action->structure, "uri"));

    if (!uri) {
      GST_VALIDATE_REPORT_ACTION (scenario, action,
          g_quark_from_string ("scenario::execution-error"),
          "None of 'uri' or 'content' passed as parametter"
          " can't load any timeline!");
      goto fail;
    }
  }

  tmp_full = ges_timeline_get_layers (timeline);
  for (tmp = tmp_full; tmp; tmp = tmp->next)
    ges_timeline_remove_layer (timeline, tmp->data);
  g_list_free_full (tmp_full, gst_object_unref);

  tmp_full = ges_timeline_get_tracks (timeline);
  for (tmp = tmp_full; tmp; tmp = tmp->next)
    ges_timeline_remove_track (timeline, tmp->data);
  g_list_free_full (tmp_full, gst_object_unref);

  project = ges_project_new (uri);
  g_signal_connect (project, "loaded", G_CALLBACK (_project_loaded_cb), action);
  ges_project_load (project, timeline, &error);
  if (error) {
    GST_VALIDATE_REPORT_ACTION (scenario, action,
        g_quark_from_string ("scenario::execution-error"),
        "Could not load timeline: %s", error->message);

    goto fail;
  }

  gst_element_set_state (pipeline, state);

done:
  if (error)
    g_error_free (error);

  if (uri)
    g_free (uri);

  g_free (tmpfile);

  return res;

fail:

  res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;

  goto done;

}

#endif

gboolean
ges_validate_register_action_types (void)
{
#ifdef HAVE_GST_VALIDATE
  gst_validate_init ();

  /*  *INDENT-OFF* */
  gst_validate_register_action_type ("edit-container", "ges", _edit_container,
      (GstValidateActionParameter [])  {
        {
         .name = "container-name",
         .description = "The name of the GESContainer to edit",
         .mandatory = TRUE,
         .types = "string",
        },
        {
          .name = "position",
          .description = "The new position of the GESContainer",
          .mandatory = TRUE,
          .types = "double or string",
          .possible_variables = "position: The current position in the stream\n"
            "duration: The duration of the stream",
           NULL
        },
        {
          .name = "edit-mode",
          .description = "The GESEditMode to use to edit @container-name",
          .mandatory = FALSE,
          .types = "string",
          .def = "normal",
        },
        {
          .name = "edge",
          .description = "The GESEdge to use to edit @container-name\n"
                         "should be in [ edge_start, edge_end, edge_none ] ",
          .mandatory = FALSE,
          .types = "string",
          .def = "edge_none",
        },
        {
          .name = "new-layer-priority",
          .description = "The priority of the layer @container should land in.\n"
                         "If the layer you're trying to move the container to doesn't exist, it will\n"
                         "be created automatically. -1 means no move.",
          .mandatory = FALSE,
          .types = "int",
          .def = "-1",
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
       },
       "Allows to edit a container (like a GESClip), for more details, have a look at:\n"
       "ges_container_edit documentation, Note that the timeline will\n"
       "be commited, and flushed so that the edition is taken into account",
       GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("add-asset", "ges", _add_asset,
      (GstValidateActionParameter [])  {
        {
          .name = "id",
          .description = "Adds an asset to a project.",
          .mandatory = TRUE,
          NULL
        },
        {
          .name = "type",
          .description = "The type of asset to add",
          .mandatory = TRUE,
          NULL
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      },
      "Allows to add an asset to the current project", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("remove-asset", "ges", _remove_asset,
      (GstValidateActionParameter [])  {
        {
          .name = "id",
          .description = "The ID of the clip to remove",
          .mandatory = TRUE,
          NULL
        },
        {
          .name = "type",
          .description = "The type of asset to remove",
          .mandatory = TRUE,
          NULL
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        { NULL }
      },
      "Allows to remove an asset from the current project", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("add-layer", "ges", _add_layer,
      (GstValidateActionParameter [])  {
        {
          .name = "priority",
          .description = "The priority of the new layer to add,"
                         "if not specified, the new layer will be"
                         " appended to the timeline",
          .mandatory = FALSE,
          NULL
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        { NULL }
      },
      "Allows to add a layer to the current timeline", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("remove-layer", "ges", _remove_layer,
      (GstValidateActionParameter [])  {
        {
          .name = "priority",
          .description = "The priority of the layer to remove",
          .mandatory = TRUE,
          NULL
        },
        {
          .name = "auto-transition",
          .description = "Wheter auto-transition is activated on the new layer.",
          .mandatory = FALSE,
          .types="boolean",
          .def = "False"
        },
        {
          .name = "project-uri",
          .description = "The nested timeline to add clip to",
          .types = "string",
          .mandatory = FALSE,
        },
        { NULL }
      },
      "Allows to remove a layer from the current timeline", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("add-clip", "ges", _validate_action_execute,
      (GstValidateActionParameter []) {
        {
          .name = "name",
          .description = "The name of the clip to add",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "layer-priority",
          .description = "The priority of the clip to add",
          .types = "int",
          .mandatory = TRUE,
        },
        {
          .name = "asset-id",
          .description = "The id of the asset from which to extract the clip",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "type",
          .description = "The type of the clip to create",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "start",
          .description = "The start value to set on the new GESClip.",
          .types = "double or string",
          .mandatory = FALSE,
        },
        {
          .name = "inpoint",
          .description = "The  inpoint value to set on the new GESClip",
          .types = "double or string",
          .mandatory = FALSE,
        },
        {
          .name = "duration",
          .description = "The  duration value to set on the new GESClip",
          .types = "double or string",
          .mandatory = FALSE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Allows to add a clip to a given layer", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("remove-clip", "ges", _remove_clip,
      (GstValidateActionParameter []) {
        {
          .name = "name",
          .description = "The name of the clip to remove",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Allows to remove a clip from a given layer", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("serialize-project", "ges", _serialize_project,
      (GstValidateActionParameter []) {
        {
          .name = "uri",
          .description = "The uri where to store the serialized project",
          .types = "string",
          .mandatory = TRUE,
        },
        {NULL}
      }, "serializes a project", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("set-child-property", "ges", _validate_action_execute,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the element on which to modify the property",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "property",
          .description = "The name of the property to modify",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "value",
          .description = "The value of the property",
          .types = "gvalue",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Allows to change child property of an object", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("check-ges-properties", "ges", set_or_check_properties,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the element on which to check properties",
          .types = "string",
          .mandatory = TRUE,
        },
        {NULL}
      }, "Check `element-name` properties values defined by the"
         " fields in the following format: `property_name=expected-value`",
        GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("check-child-properties", "ges", set_or_check_properties,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the element on which to check children properties",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "at-time",
          .description = "The time at which to check the values, taking into"
            " account the ControlBinding if any set.",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Check `element-name` children properties values defined by the"
         " fields in the following format: `property_name=expected-value`",
        GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("set-child-properties", "ges", set_or_check_properties,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the element on which to modify child properties",
          .types = "string",
          .mandatory = TRUE,
        },
        {NULL}
      }, "Sets `element-name` children properties values defined by the"
         " fields in the following format: `property-name=new-value`",
        GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("split-clip", "ges", _split_clip,
      (GstValidateActionParameter []) {
        {
          .name = "clip-name",
          .description = "The name of the clip to split",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "position",
          .description = "The position at which to split the clip",
          .types = "double or string",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Split a clip at a specified position.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("set-track-restriction-caps", "ges", _set_track_restriction_caps,
      (GstValidateActionParameter []) {
        {
          .name = "track-type",
          .description = "The type of track to set restriction caps on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "caps",
          .description = "The caps to set on the track",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Sets restriction caps on tracks of a specific type.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("element-set-asset", "ges", _set_asset_on_element,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the TimelineElement to set an asset on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "asset-id",
          .description = "The id of the asset from which to extract the clip",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Sets restriction caps on tracks of a specific type.", GST_VALIDATE_ACTION_TYPE_NONE);


  gst_validate_register_action_type ("container-add-child", "ges", _validate_action_execute,
      (GstValidateActionParameter []) {
        {
          .name = "container-name",
          .description = "The name of the GESContainer to add a child to",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "child-name",
          .description = "The name of the child to add to @container-name",
          .types = "string",
          .mandatory = FALSE,
          .def = "NULL"
        },
        {
          .name = "asset-id",
          .description = "The id of the asset from which to extract the child",
          .types = "string",
          .mandatory = TRUE,
          .def = "NULL"
        },
        {
          .name = "child-type",
          .description = "The type of the child to create",
          .types = "string",
          .mandatory = FALSE,
          .def = "NULL"
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Add a child to @container-name. If asset-id and child-type are specified,"
       " the child will be created and added. Otherwize @child-name has to be specified"
       " and will be added to the container.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("container-remove-child", "ges", _container_remove_child,
      (GstValidateActionParameter []) {
        {
          .name = "container-name",
          .description = "The name of the GESContainer to remove a child from",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "child-name",
          .description = "The name of the child to reomve from @container-name",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Remove a child from @container-name.", FALSE);

  gst_validate_register_action_type ("ungroup-container", "ges", _ungroup,
      (GstValidateActionParameter []) {
        {
          .name = "container-name",
          .description = "The name of the GESContainer to ungroup children from",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "recursive",
          .description = "Wether to recurse ungrouping or not.",
          .types = "boolean",
          .mandatory = FALSE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Ungroup children of @container-name.", FALSE);

  gst_validate_register_action_type ("set-control-source", "ges", _set_control_source,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the GESTrackElement to set the control source on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "property-name",
          .description = "The name of the property for which to set a control source",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "binding-type",
          .description = "The name of the type of binding to use",
          .types = "string",
          .mandatory = FALSE,
          .def = "direct",
        },
        {
          .name = "source-type",
          .description = "The name of the type of ControlSource to use",
          .types = "string",
          .mandatory = FALSE,
          .def = "interpolation",
        },
        {
          .name = "interpolation-mode",
          .description = "The name of the GstInterpolationMode to on the source",
          .types = "string",
          .mandatory = FALSE,
          .def = "linear",
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Adds a GstControlSource on @element-name::@property-name"
         " allowing you to then add keyframes on that property.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("add-keyframe", "ges", _validate_action_execute,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the GESTrackElement to add a keyframe on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "property-name",
          .description = "The name of the property for which to add a keyframe on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "timestamp",
          .description = "The timestamp of the keyframe",
          .types = "string or float",
          .mandatory = TRUE,
        },
        {
          .name = "value",
          .description = "The value of the keyframe",
          .types = "float",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Remove a child from @container-name.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("copy-element", "ges", _copy_element,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the GESTtimelineElement to copy",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "recurse",
          .description = "Copy recursively or not",
          .types = "boolean",
          .def = "true",
          .mandatory = FALSE,
        },
        {
          .name = "position",
          .description = "The time where to paste the element",
          .types = "string or float",
          .mandatory = TRUE,
        },
        {
          .name = "paste-name",
          .description = "The name of the copied element",
          .types = "string",
          .mandatory = FALSE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Remove a child from @container-name.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("remove-keyframe", "ges", _validate_action_execute,
      (GstValidateActionParameter []) {
        {
          .name = "element-name",
          .description = "The name of the GESTrackElement to add a keyframe on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "property-name",
          .description = "The name of the property for which to add a keyframe on",
          .types = "string",
          .mandatory = TRUE,
        },
        {
          .name = "timestamp",
          .description = "The timestamp of the keyframe",
          .types = "string or float",
          .mandatory = TRUE,
        },
        {
          .name = "project-uri",
          .description = "The project URI with the serialized timeline to execute the action on",
          .types = "string",
          .mandatory = FALSE,
        },
        {NULL}
      }, "Remove a child from @container-name.", GST_VALIDATE_ACTION_TYPE_NONE);

  gst_validate_register_action_type ("load-project", "ges", _load_project,
      (GstValidateActionParameter [])  {
        {
          .name = "serialized-content",
          .description = "The full content of the XML describing project in XGES format.",
          .mandatory = FALSE,
          .types = "string",
          NULL
        },
        {
          .name = "uri",
          .description = "The uri of the project to load (used only if serialized-content is not provided)",
          .mandatory = FALSE,
          .types = "string",
          NULL
        },
        {NULL}
      },
      "Loads a project either from its content passed in the 'serialized-content' field or using the provided 'uri'.\n"
      "Note that it will completely clean the previous timeline",
      GST_VALIDATE_ACTION_TYPE_NONE);


  gst_validate_register_action_type ("commit", "ges", _commit, NULL,
       "Commit the timeline.", GST_VALIDATE_ACTION_TYPE_ASYNC);
  /*  *INDENT-ON* */

  return TRUE;
#else
  return FALSE;
#endif
}