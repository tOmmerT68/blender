/* SPDX-FileCopyrightText: 2008 Blender Authors, Joshua Leung. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edanimation
 */

/* This file contains a system used to provide a layer of abstraction between sources
 * of animation data and tools in Animation Editors. The method used here involves
 * generating a list of edit structures which enable tools to naively perform the actions
 * they require without all the boiler-plate associated with loops within loops and checking
 * for cases to ignore.
 *
 * While this is primarily used for the Action/Dopesheet Editor (and its accessory modes),
 * the Graph Editor also uses this for its channel list and for determining which curves
 * are being edited. Likewise, the NLA Editor also uses this for its channel list and in
 * its operators.
 *
 * NOTE: much of the original system this was based on was built before the creation of the RNA
 * system. In future, it would be interesting to replace some parts of this code with RNA queries,
 * however, RNA does not eliminate some of the boiler-plate reduction benefits presented by this
 * system, so if any such work does occur, it should only be used for the internals used here...
 *
 * -- Joshua Leung, Dec 2008 (Last revision July 2009)
 */

#include <cstring>

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_brush_types.h"
#include "DNA_cachefile_types.h"
#include "DNA_camera_types.h"
#include "DNA_curves_types.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_key_types.h"
#include "DNA_lattice_types.h"
#include "DNA_layer_types.h"
#include "DNA_light_types.h"
#include "DNA_linestyle_types.h"
#include "DNA_mask_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_node_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"
#include "DNA_speaker_types.h"
#include "DNA_userdef_types.h"
#include "DNA_volume_types.h"
#include "DNA_world_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_alloca.h"
#include "BLI_ghash.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_action.h"
#include "BKE_anim_data.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_fcurve.h"
#include "BKE_fcurve_driver.h"
#include "BKE_global.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_mask.h"
#include "BKE_material.h"
#include "BKE_modifier.hh"
#include "BKE_node.hh"

#include "ED_anim_api.hh"
#include "ED_markers.hh"

#include "SEQ_sequencer.hh"
#include "SEQ_utils.hh"

#include "ANIM_bone_collections.hh"

/* ************************************************************ */
/* Blender Context <-> Animation Context mapping */

/* ----------- Private Stuff - Action Editor ------------- */

/* Get shapekey data being edited (for Action Editor -> ShapeKey mode) */
/* NOTE: there's a similar function in `key.cc` #BKE_key_from_object. */
static Key *actedit_get_shapekeys(bAnimContext *ac)
{
  Scene *scene = ac->scene;
  ViewLayer *view_layer = ac->view_layer;
  Object *ob;
  Key *key;

  BKE_view_layer_synced_ensure(scene, view_layer);
  ob = BKE_view_layer_active_object_get(view_layer);
  if (ob == nullptr) {
    return nullptr;
  }

  /* XXX pinning is not available in 'ShapeKey' mode... */
  // if (saction->pin) { return nullptr; }

  /* shapekey data is stored with geometry data */
  key = BKE_key_from_object(ob);

  if (key) {
    if (key->type == KEY_RELATIVE) {
      return key;
    }
  }

  return nullptr;
}

/* Get data being edited in Action Editor (depending on current 'mode') */
static bool actedit_get_context(bAnimContext *ac, SpaceAction *saction)
{
  /* get dopesheet */
  ac->ads = &saction->ads;

  /* sync settings with current view status, then return appropriate data */
  switch (saction->mode) {
    case SACTCONT_ACTION: /* 'Action Editor' */
      /* if not pinned, sync with active object */
      if (/* `saction->pin == 0` */ true) {
        if (ac->obact && ac->obact->adt) {
          saction->action = ac->obact->adt->action;
        }
        else {
          saction->action = nullptr;
        }
      }

      ac->datatype = ANIMCONT_ACTION;
      ac->data = saction->action;

      ac->mode = saction->mode;
      return true;

    case SACTCONT_SHAPEKEY: /* 'ShapeKey Editor' */
      ac->datatype = ANIMCONT_SHAPEKEY;
      ac->data = actedit_get_shapekeys(ac);

      /* if not pinned, sync with active object */
      if (/* `saction->pin == 0` */ true) {
        Key *key = (Key *)ac->data;

        if (key && key->adt) {
          saction->action = key->adt->action;
        }
        else {
          saction->action = nullptr;
        }
      }

      ac->mode = saction->mode;
      return true;

    case SACTCONT_GPENCIL: /* Grease Pencil */ /* XXX review how this mode is handled... */
      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      saction->ads.source = (ID *)ac->scene;

      ac->datatype = ANIMCONT_GPENCIL;
      ac->data = &saction->ads;

      ac->mode = saction->mode;
      return true;

    case SACTCONT_CACHEFILE: /* Cache File */ /* XXX review how this mode is handled... */
      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      saction->ads.source = (ID *)ac->scene;

      ac->datatype = ANIMCONT_CHANNEL;
      ac->data = &saction->ads;

      ac->mode = saction->mode;
      return true;

    case SACTCONT_MASK: /* Mask */ /* XXX: review how this mode is handled. */
    {
      /* TODO: other methods to get the mask. */
#if 0
      Sequence *seq = SEQ_select_active_get(ac->scene);
      MovieClip *clip = ac->scene->clip;
      struct Mask *mask = seq ? seq->mask : nullptr;
#endif

      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      saction->ads.source = (ID *)ac->scene;

      ac->datatype = ANIMCONT_MASK;
      ac->data = &saction->ads;

      ac->mode = saction->mode;
      return true;
    }

    case SACTCONT_DOPESHEET: /* DopeSheet */
      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      saction->ads.source = (ID *)ac->scene;

      ac->datatype = ANIMCONT_DOPESHEET;
      ac->data = &saction->ads;

      ac->mode = saction->mode;
      return true;

    case SACTCONT_TIMELINE: /* Timeline */
      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      saction->ads.source = (ID *)ac->scene;

      /* sync scene's "selected keys only" flag with our "only selected" flag
       *
       * XXX: This is a workaround for #55525. We shouldn't really be syncing the flags like this,
       * but it's a simpler fix for now than also figuring out how the next/prev keyframe
       * tools should work in the 3D View if we allowed full access to the timeline's
       * dopesheet filters (i.e. we'd have to figure out where to host those settings,
       * to be on a scene level like this flag currently is, along with several other unknowns).
       */
      if (ac->scene->flag & SCE_KEYS_NO_SELONLY) {
        saction->ads.filterflag &= ~ADS_FILTER_ONLYSEL;
      }
      else {
        saction->ads.filterflag |= ADS_FILTER_ONLYSEL;
      }

      ac->datatype = ANIMCONT_TIMELINE;
      ac->data = &saction->ads;

      ac->mode = saction->mode;
      return true;

    default: /* unhandled yet */
      ac->datatype = ANIMCONT_NONE;
      ac->data = nullptr;

      ac->mode = -1;
      return false;
  }
}

/* ----------- Private Stuff - Graph Editor ------------- */

/* Get data being edited in Graph Editor (depending on current 'mode') */
static bool graphedit_get_context(bAnimContext *ac, SpaceGraph *sipo)
{
  /* init dopesheet data if non-existent (i.e. for old files) */
  if (sipo->ads == nullptr) {
    sipo->ads = static_cast<bDopeSheet *>(MEM_callocN(sizeof(bDopeSheet), "GraphEdit DopeSheet"));
    sipo->ads->source = (ID *)ac->scene;
  }
  ac->ads = sipo->ads;

  /* set settings for Graph Editor - "Selected = Editable" */
  if (U.animation_flag & USER_ANIM_ONLY_SHOW_SELECTED_CURVE_KEYS) {
    sipo->ads->filterflag |= ADS_FILTER_SELEDIT;
  }
  else {
    sipo->ads->filterflag &= ~ADS_FILTER_SELEDIT;
  }

  /* sync settings with current view status, then return appropriate data */
  switch (sipo->mode) {
    case SIPO_MODE_ANIMATION: /* Animation F-Curve Editor */
      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      sipo->ads->source = (ID *)ac->scene;
      sipo->ads->filterflag &= ~ADS_FILTER_ONLYDRIVERS;

      ac->datatype = ANIMCONT_FCURVES;
      ac->data = sipo->ads;

      ac->mode = sipo->mode;
      return true;

    case SIPO_MODE_DRIVERS: /* Driver F-Curve Editor */
      /* update scene-pointer (no need to check for pinning yet, as not implemented) */
      sipo->ads->source = (ID *)ac->scene;
      sipo->ads->filterflag |= ADS_FILTER_ONLYDRIVERS;

      ac->datatype = ANIMCONT_DRIVERS;
      ac->data = sipo->ads;

      ac->mode = sipo->mode;
      return true;

    default: /* unhandled yet */
      ac->datatype = ANIMCONT_NONE;
      ac->data = nullptr;

      ac->mode = -1;
      return false;
  }
}

/* ----------- Private Stuff - NLA Editor ------------- */

/* Get data being edited in Graph Editor (depending on current 'mode') */
static bool nlaedit_get_context(bAnimContext *ac, SpaceNla *snla)
{
  /* init dopesheet data if non-existent (i.e. for old files) */
  if (snla->ads == nullptr) {
    snla->ads = static_cast<bDopeSheet *>(MEM_callocN(sizeof(bDopeSheet), "NlaEdit DopeSheet"));
  }
  ac->ads = snla->ads;

  /* sync settings with current view status, then return appropriate data */
  /* update scene-pointer (no need to check for pinning yet, as not implemented) */
  snla->ads->source = (ID *)ac->scene;
  snla->ads->filterflag |= ADS_FILTER_ONLYNLA;

  ac->datatype = ANIMCONT_NLA;
  ac->data = snla->ads;

  return true;
}

/* ----------- Public API --------------- */

bool ANIM_animdata_context_getdata(bAnimContext *ac)
{
  SpaceLink *sl = ac->sl;
  bool ok = false;

  /* context depends on editor we are currently in */
  if (sl) {
    switch (ac->spacetype) {
      case SPACE_ACTION: {
        SpaceAction *saction = (SpaceAction *)sl;
        ok = actedit_get_context(ac, saction);
        break;
      }
      case SPACE_GRAPH: {
        SpaceGraph *sipo = (SpaceGraph *)sl;
        ok = graphedit_get_context(ac, sipo);
        break;
      }
      case SPACE_NLA: {
        SpaceNla *snla = (SpaceNla *)sl;
        ok = nlaedit_get_context(ac, snla);
        break;
      }
    }
  }

  /* check if there's any valid data */
  return (ok && ac->data);
}

bool ANIM_animdata_get_context(const bContext *C, bAnimContext *ac)
{
  Main *bmain = CTX_data_main(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  SpaceLink *sl = CTX_wm_space_data(C);
  Scene *scene = CTX_data_scene(C);

  /* clear old context info */
  if (ac == nullptr) {
    return false;
  }
  memset(ac, 0, sizeof(bAnimContext));

  /* get useful default context settings from context */
  ac->bmain = bmain;
  ac->scene = scene;
  ac->view_layer = CTX_data_view_layer(C);
  if (scene) {
    ac->markers = ED_context_get_markers(C);
    BKE_view_layer_synced_ensure(ac->scene, ac->view_layer);
  }
  ac->depsgraph = CTX_data_depsgraph_pointer(C);
  ac->obact = BKE_view_layer_active_object_get(ac->view_layer);
  ac->area = area;
  ac->region = region;
  ac->sl = sl;
  ac->spacetype = (area) ? area->spacetype : 0;
  ac->regiontype = (region) ? region->regiontype : 0;

  /* get data context info */
  /* XXX: if the below fails, try to grab this info from context instead...
   * (to allow for scripting). */
  return ANIM_animdata_context_getdata(ac);
}

bool ANIM_animdata_can_have_greasepencil(const eAnimCont_Types type)
{
  return ELEM(type, ANIMCONT_GPENCIL, ANIMCONT_DOPESHEET, ANIMCONT_TIMELINE);
}

/* ************************************************************ */
/* Blender Data <-- Filter --> Channels to be operated on */

/* macros to use before/after getting the sub-channels of some channel,
 * to abstract away some of the tricky logic involved
 *
 * cases:
 * 1) Graph Edit main area (just data) OR channels visible in Channel List
 * 2) If not showing channels, we're only interested in the data (Action Editor's editing)
 * 3) We don't care what data, we just care there is some (so that a collapsed
 *    channel can be kept around). No need to clear channels-flag in order to
 *    keep expander channels with no sub-data out, as those cases should get
 *    dealt with by the recursive detection idiom in place.
 *
 * Implementation NOTE:
 *  YES the _doSubChannels variable is NOT read anywhere. BUT, this is NOT an excuse
 *  to go steamrolling the logic into a single-line expression as from experience,
 *  those are notoriously difficult to read + debug when extending later on. The code
 *  below is purposefully laid out so that each case noted above corresponds clearly to
 *  one case below.
 */
#define BEGIN_ANIMFILTER_SUBCHANNELS(expanded_check) \
  { \
    int _filter = filter_mode; \
    short _doSubChannels = 0; \
    if (!(filter_mode & ANIMFILTER_LIST_VISIBLE) || (expanded_check)) { \
      _doSubChannels = 1; \
    } \
    else if (!(filter_mode & ANIMFILTER_LIST_CHANNELS)) { \
      _doSubChannels = 2; \
    } \
    else { \
      filter_mode |= ANIMFILTER_TMP_PEEK; \
    } \
\
    { \
      (void)_doSubChannels; \
    }
/* ... standard sub-channel filtering can go on here now ... */
#define END_ANIMFILTER_SUBCHANNELS \
  filter_mode = _filter; \
  } \
  (void)0

/* ............................... */

/* quick macro to test if AnimData is usable */
#define ANIMDATA_HAS_KEYS(id) ((id)->adt && (id)->adt->action)

/* quick macro to test if AnimData is usable for drivers */
#define ANIMDATA_HAS_DRIVERS(id) ((id)->adt && (id)->adt->drivers.first)

/* quick macro to test if AnimData is usable for NLA */
#define ANIMDATA_HAS_NLA(id) ((id)->adt && (id)->adt->nla_tracks.first)

/**
 * Quick macro to test for all three above usability tests, performing the appropriate provided
 * action for each when the AnimData context is appropriate.
 *
 * Priority order for this goes (most important, to least):
 * AnimData blocks, NLA, Drivers, Keyframes.
 *
 * For this to work correctly,
 * a standard set of data needs to be available within the scope that this
 *
 * Gets called in:
 * - ListBase anim_data;
 * - bDopeSheet *ads;
 * - bAnimListElem *ale;
 * - size_t items;
 *
 * - id: ID block which should have an AnimData pointer following it immediately, to use
 * - adtOk: line or block of code to execute for AnimData-blocks case
 *   (usually #ANIMDATA_ADD_ANIMDATA).
 * - nlaOk: line or block of code to execute for NLA tracks+strips case
 * - driversOk: line or block of code to execute for Drivers case
 * - nlaKeysOk: line or block of code for NLA Strip Keyframes case
 * - keysOk: line or block of code for Keyframes case
 *
 * The checks for the various cases are as follows:
 * 0) top level: checks for animdata and also that all the F-Curves for the block will be visible
 * 1) animdata check: for filtering animdata blocks only
 * 2A) nla tracks: include animdata block's data as there are NLA tracks+strips there
 * 2B) actions to convert to nla: include animdata block's data as there is an action that can be
 *     converted to a new NLA strip, and the filtering options allow this
 * 2C) allow non-animated data-blocks to be included so that data-blocks can be added
 * 3) drivers: include drivers from animdata block (for Drivers mode in Graph Editor)
 * 4A) nla strip keyframes: these are the per-strip controls for time and influence
 * 4B) normal keyframes: only when there is an active action
 */
#define ANIMDATA_FILTER_CASES(id, adtOk, nlaOk, driversOk, nlaKeysOk, keysOk) \
  { \
    if ((id)->adt) { \
      if (!(filter_mode & ANIMFILTER_CURVE_VISIBLE) || \
          !((id)->adt->flag & ADT_CURVES_NOT_VISIBLE)) { \
        if (filter_mode & ANIMFILTER_ANIMDATA) { \
          adtOk \
        } \
        else if (ads->filterflag & ADS_FILTER_ONLYNLA) { \
          if (ANIMDATA_HAS_NLA(id)) { \
            nlaOk \
          } \
          else if (!(ads->filterflag & ADS_FILTER_NLA_NOACT) || ANIMDATA_HAS_KEYS(id)) { \
            nlaOk \
          } \
        } \
        else if (ads->filterflag & ADS_FILTER_ONLYDRIVERS) { \
          if (ANIMDATA_HAS_DRIVERS(id)) { \
            driversOk \
          } \
        } \
        else { \
          if (ANIMDATA_HAS_NLA(id)) { \
            nlaKeysOk \
          } \
          if (ANIMDATA_HAS_KEYS(id)) { \
            keysOk \
          } \
        } \
      } \
    } \
  } \
  (void)0

/* ............................... */

/**
 * Add a new animation channel, taking into account the "peek" flag, which is used to just check
 * whether any channels will be added (but without needing them to actually get created).
 *
 * \warning This causes the calling function to return early if we're only "peeking" for channels.
 *
 * XXX: ale_statement stuff is really a hack for one special case. It shouldn't really be needed.
 */
#define ANIMCHANNEL_NEW_CHANNEL_FULL( \
    channel_data, channel_type, owner_id, fcurve_owner_id, ale_statement) \
  if (filter_mode & ANIMFILTER_TMP_PEEK) { \
    return 1; \
  } \
  { \
    bAnimListElem *ale = make_new_animlistelem( \
        channel_data, channel_type, (ID *)owner_id, fcurve_owner_id); \
    if (ale) { \
      BLI_addtail(anim_data, ale); \
      items++; \
      ale_statement \
    } \
  } \
  (void)0

#define ANIMCHANNEL_NEW_CHANNEL(channel_data, channel_type, owner_id, fcurve_owner_id) \
  ANIMCHANNEL_NEW_CHANNEL_FULL(channel_data, channel_type, owner_id, fcurve_owner_id, {})

/* ............................... */

/* quick macro to test if an anim-channel representing an AnimData block is suitably active */
#define ANIMCHANNEL_ACTIVEOK(ale) \
  (!(filter_mode & ANIMFILTER_ACTIVE) || !(ale->adt) || (ale->adt->flag & ADT_UI_ACTIVE))

/* Quick macro to test if an anim-channel (F-Curve, Group, etc.)
 * is selected in an acceptable way. */
#define ANIMCHANNEL_SELOK(test_func) \
  (!(filter_mode & (ANIMFILTER_SEL | ANIMFILTER_UNSEL)) || \
   ((filter_mode & ANIMFILTER_SEL) && test_func) || \
   ((filter_mode & ANIMFILTER_UNSEL) && test_func == 0))

/**
 * Quick macro to test if an anim-channel (F-Curve) is selected ok for editing purposes
 * - `*_SELEDIT` means that only selected curves will have visible+editable key-frames.
 *
 * checks here work as follows:
 * 1) SELEDIT off - don't need to consider the implications of this option.
 * 2) FOREDIT off - we're not considering editing, so channel is ok still.
 * 3) test_func (i.e. selection test) - only if selected, this test will pass.
 */
#define ANIMCHANNEL_SELEDITOK(test_func) \
  (!(filter_mode & ANIMFILTER_SELEDIT) || !(filter_mode & ANIMFILTER_FOREDIT) || (test_func))

/* ----------- 'Private' Stuff --------------- */

/* this function allocates memory for a new bAnimListElem struct for the
 * provided animation channel-data.
 */
static bAnimListElem *make_new_animlistelem(void *data,
                                            short datatype,
                                            ID *owner_id,
                                            ID *fcurve_owner_id)
{
  bAnimListElem *ale = nullptr;

  /* only allocate memory if there is data to convert */
  if (data) {
    /* allocate and set generic data */
    ale = static_cast<bAnimListElem *>(MEM_callocN(sizeof(bAnimListElem), "bAnimListElem"));

    ale->data = data;
    ale->type = datatype;

    ale->id = owner_id;
    ale->adt = BKE_animdata_from_id(owner_id);
    ale->fcurve_owner_id = fcurve_owner_id;

    /* do specifics */
    switch (datatype) {
      case ANIMTYPE_SUMMARY: {
        /* Nothing to include for now... this is just a dummy wrapper around
         * all the other channels in the DopeSheet, and gets included at the start of the list. */
        ale->key_data = nullptr;
        ale->datatype = ALE_ALL;
        break;
      }
      case ANIMTYPE_SCENE: {
        Scene *sce = (Scene *)data;

        ale->flag = sce->flag;

        ale->key_data = sce;
        ale->datatype = ALE_SCE;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_OBJECT: {
        Base *base = (Base *)data;
        Object *ob = base->object;

        ale->flag = ob->flag;

        ale->key_data = ob;
        ale->datatype = ALE_OB;

        ale->adt = BKE_animdata_from_id(&ob->id);
        break;
      }
      case ANIMTYPE_FILLACTD: {
        bAction *act = (bAction *)data;

        ale->flag = act->flag;

        ale->key_data = act;
        ale->datatype = ALE_ACT;
        break;
      }
      case ANIMTYPE_FILLDRIVERS: {
        AnimData *adt = (AnimData *)data;

        ale->flag = adt->flag;

        /* XXX drivers don't show summary for now. */
        ale->key_data = nullptr;
        ale->datatype = ALE_NONE;
        break;
      }
      case ANIMTYPE_DSMAT: {
        Material *ma = (Material *)data;
        AnimData *adt = ma->adt;

        ale->flag = FILTER_MAT_OBJD(ma);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSLAM: {
        Light *la = (Light *)data;
        AnimData *adt = la->adt;

        ale->flag = FILTER_LAM_OBJD(la);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSCAM: {
        Camera *ca = (Camera *)data;
        AnimData *adt = ca->adt;

        ale->flag = FILTER_CAM_OBJD(ca);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSCACHEFILE: {
        CacheFile *cache_file = (CacheFile *)data;
        AnimData *adt = cache_file->adt;

        ale->flag = FILTER_CACHEFILE_OBJD(cache_file);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSCUR: {
        Curve *cu = (Curve *)data;
        AnimData *adt = cu->adt;

        ale->flag = FILTER_CUR_OBJD(cu);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSARM: {
        bArmature *arm = (bArmature *)data;
        AnimData *adt = arm->adt;

        ale->flag = FILTER_ARM_OBJD(arm);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSMESH: {
        Mesh *mesh = (Mesh *)data;
        AnimData *adt = mesh->adt;

        ale->flag = FILTER_MESH_OBJD(mesh);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSLAT: {
        Lattice *lt = (Lattice *)data;
        AnimData *adt = lt->adt;

        ale->flag = FILTER_LATTICE_OBJD(lt);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSSPK: {
        Speaker *spk = (Speaker *)data;
        AnimData *adt = spk->adt;

        ale->flag = FILTER_SPK_OBJD(spk);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSHAIR: {
        Curves *curves = (Curves *)data;
        AnimData *adt = curves->adt;

        ale->flag = FILTER_CURVES_OBJD(curves);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSPOINTCLOUD: {
        PointCloud *pointcloud = (PointCloud *)data;
        AnimData *adt = pointcloud->adt;

        ale->flag = FILTER_POINTS_OBJD(pointcloud);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSVOLUME: {
        Volume *volume = (Volume *)data;
        AnimData *adt = volume->adt;

        ale->flag = FILTER_VOLUME_OBJD(volume);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSSKEY: {
        Key *key = (Key *)data;
        AnimData *adt = key->adt;

        ale->flag = FILTER_SKE_OBJD(key);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSWOR: {
        World *wo = (World *)data;
        AnimData *adt = wo->adt;

        ale->flag = FILTER_WOR_SCED(wo);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSNTREE: {
        bNodeTree *ntree = (bNodeTree *)data;
        AnimData *adt = ntree->adt;

        ale->flag = FILTER_NTREE_DATA(ntree);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSLINESTYLE: {
        FreestyleLineStyle *linestyle = (FreestyleLineStyle *)data;
        AnimData *adt = linestyle->adt;

        ale->flag = FILTER_LS_SCED(linestyle);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSPART: {
        ParticleSettings *part = (ParticleSettings *)ale->data;
        AnimData *adt = part->adt;

        ale->flag = FILTER_PART_OBJD(part);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSTEX: {
        Tex *tex = (Tex *)data;
        AnimData *adt = tex->adt;

        ale->flag = FILTER_TEX_DATA(tex);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSGPENCIL: {
        bGPdata *gpd = (bGPdata *)data;
        AnimData *adt = gpd->adt;

        /* NOTE: we just reuse the same expand filter for this case */
        ale->flag = EXPANDED_GPD(gpd);

        /* XXX: currently, this is only used for access to its animation data */
        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_DSMCLIP: {
        MovieClip *clip = (MovieClip *)data;
        AnimData *adt = clip->adt;

        ale->flag = EXPANDED_MCLIP(clip);

        ale->key_data = (adt) ? adt->action : nullptr;
        ale->datatype = ALE_ACT;

        ale->adt = BKE_animdata_from_id(static_cast<ID *>(data));
        break;
      }
      case ANIMTYPE_NLACONTROLS: {
        AnimData *adt = (AnimData *)data;

        ale->flag = adt->flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_NONE;
        break;
      }
      case ANIMTYPE_GROUP: {
        bActionGroup *agrp = (bActionGroup *)data;

        ale->flag = agrp->flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_GROUP;
        break;
      }
      case ANIMTYPE_FCURVE:
      case ANIMTYPE_NLACURVE: /* practically the same as ANIMTYPE_FCURVE.
                               * Differences are applied post-creation */
      {
        FCurve *fcu = (FCurve *)data;

        ale->flag = fcu->flag;

        ale->key_data = fcu;
        ale->datatype = ALE_FCURVE;
        break;
      }
      case ANIMTYPE_SHAPEKEY: {
        KeyBlock *kb = (KeyBlock *)data;
        Key *key = (Key *)ale->id;

        ale->flag = kb->flag;

        /* whether we have keyframes depends on whether there is a Key block to find it from */
        if (key) {
          /* index of shapekey is defined by place in key's list */
          ale->index = BLI_findindex(&key->block, kb);

          /* the corresponding keyframes are from the animdata */
          if (ale->adt && ale->adt->action) {
            bAction *act = ale->adt->action;
            /* Try to find the F-Curve which corresponds to this exactly. */
            if (std::optional<std::string> rna_path = BKE_keyblock_curval_rnapath_get(key, kb)) {
              ale->key_data = (void *)BKE_fcurve_find(&act->curves, rna_path->c_str(), 0);
            }
          }
          ale->datatype = (ale->key_data) ? ALE_FCURVE : ALE_NONE;
        }
        break;
      }
      case ANIMTYPE_GPLAYER: {
        bGPDlayer *gpl = (bGPDlayer *)data;

        ale->flag = gpl->flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_GPFRAME;
        break;
      }
      case ANIMTYPE_GREASE_PENCIL_LAYER: {
        GreasePencilLayer *layer = static_cast<GreasePencilLayer *>(data);

        ale->flag = layer->base.flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_GREASE_PENCIL_CEL;
        break;
      }
      case ANIMTYPE_GREASE_PENCIL_LAYER_GROUP: {
        GreasePencilLayerTreeGroup *layer_group = static_cast<GreasePencilLayerTreeGroup *>(data);

        ale->flag = layer_group->base.flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_GREASE_PENCIL_GROUP;
        break;
      }
      case ANIMTYPE_GREASE_PENCIL_DATABLOCK: {
        GreasePencil *grease_pencil = static_cast<GreasePencil *>(data);

        ale->flag = grease_pencil->flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_GREASE_PENCIL_DATA;
        break;
      }
      case ANIMTYPE_MASKLAYER: {
        MaskLayer *masklay = (MaskLayer *)data;

        ale->flag = masklay->flag;

        ale->key_data = nullptr;
        ale->datatype = ALE_MASKLAY;
        break;
      }
      case ANIMTYPE_NLATRACK: {
        NlaTrack *nlt = (NlaTrack *)data;

        ale->flag = nlt->flag;

        ale->key_data = &nlt->strips;
        ale->datatype = ALE_NLASTRIP;
        break;
      }
      case ANIMTYPE_NLAACTION: {
        /* nothing to include for now... nothing editable from NLA-perspective here */
        ale->key_data = nullptr;
        ale->datatype = ALE_NONE;
        break;
      }
    }
  }

  /* return created datatype */
  return ale;
}

/* ----------------------------------------- */

/* 'Only Selected' selected data and/or 'Include Hidden' filtering
 * NOTE: when this function returns true, the F-Curve is to be skipped
 */
static bool skip_fcurve_selected_data(bDopeSheet *ads, FCurve *fcu, ID *owner_id, int filter_mode)
{
  if (fcu->grp != nullptr && fcu->grp->flag & ADT_CURVES_ALWAYS_VISIBLE) {
    return false;
  }
  /* hidden items should be skipped if we only care about visible data,
   * but we aren't interested in hidden stuff */
  const bool skip_hidden = (filter_mode & ANIMFILTER_DATA_VISIBLE) &&
                           !(ads->filterflag & ADS_FILTER_INCL_HIDDEN);

  if (GS(owner_id->name) == ID_OB) {
    Object *ob = (Object *)owner_id;
    bPoseChannel *pchan = nullptr;
    char bone_name[sizeof(pchan->name)];

    /* Only consider if F-Curve involves `pose.bones`. */
    if (fcu->rna_path &&
        BLI_str_quoted_substr(fcu->rna_path, "pose.bones[", bone_name, sizeof(bone_name)))
    {
      /* Get bone-name, and check if this bone is selected. */
      pchan = BKE_pose_channel_find_name(ob->pose, bone_name);

      /* check whether to continue or skip */
      if (pchan && pchan->bone) {
        /* If only visible channels,
         * skip if bone not visible unless user wants channels from hidden data too. */
        if (skip_hidden) {
          bArmature *arm = (bArmature *)ob->data;

          /* skipping - not visible on currently visible layers */
          if (!ANIM_bonecoll_is_visible_pchan(arm, pchan)) {
            return true;
          }
          /* skipping - is currently hidden */
          if (pchan->bone->flag & BONE_HIDDEN_P) {
            return true;
          }
        }

        /* can only add this F-Curve if it is selected */
        if (ads->filterflag & ADS_FILTER_ONLYSEL) {
          if ((pchan->bone->flag & BONE_SELECTED) == 0) {
            return true;
          }
        }
      }
    }
  }
  else if (GS(owner_id->name) == ID_SCE) {
    Scene *scene = (Scene *)owner_id;
    Sequence *seq = nullptr;
    char seq_name[sizeof(seq->name)];

    /* Only consider if F-Curve involves `sequence_editor.sequences`. */
    if (fcu->rna_path &&
        BLI_str_quoted_substr(fcu->rna_path, "sequences_all[", seq_name, sizeof(seq_name)))
    {
      /* Get strip name, and check if this strip is selected. */
      Editing *ed = SEQ_editing_get(scene);
      if (ed) {
        seq = SEQ_get_sequence_by_name(ed->seqbasep, seq_name, false);
      }

      /* Can only add this F-Curve if it is selected. */
      if (ads->filterflag & ADS_FILTER_ONLYSEL) {

        /* NOTE(@ideasman42): The `seq == nullptr` check doesn't look right
         * (compared to other checks in this function which skip data that can't be found).
         *
         * This is done since the search for sequence strips doesn't use a global lookup:
         * - Nested meta-strips are excluded.
         * - When inside a meta-strip - strips outside the meta-strip excluded.
         *
         * Instead, only the strips directly visible to the user are considered for selection.
         * The nullptr check here means everything else is considered unselected and is not shown.
         *
         * There is a subtle difference between nodes, pose-bones ... etc
         * since data-paths that point to missing strips are not shown.
         * If this is an important difference, the nullptr case could perform a global lookup,
         * only returning `true` if the sequence strip exists elsewhere
         * (ignoring its selection state). */
        if (seq == nullptr) {
          return true;
        }

        if ((seq->flag & SELECT) == 0) {
          return true;
        }
      }
    }
  }
  else if (GS(owner_id->name) == ID_NT) {
    bNodeTree *ntree = (bNodeTree *)owner_id;
    bNode *node = nullptr;
    char node_name[sizeof(node->name)];

    /* Check for selected nodes. */
    if (fcu->rna_path &&
        BLI_str_quoted_substr(fcu->rna_path, "nodes[", node_name, sizeof(node_name)))
    {
      /* Get strip name, and check if this strip is selected. */
      node = nodeFindNodebyName(ntree, node_name);

      /* Can only add this F-Curve if it is selected. */
      if (node) {
        if (ads->filterflag & ADS_FILTER_ONLYSEL) {
          if ((node->flag & NODE_SELECT) == 0) {
            return true;
          }
        }
      }
    }
  }

  return false;
}

/* Helper for name-based filtering - Perform "partial/fuzzy matches" (as in 80a7efd) */
static bool name_matches_dopesheet_filter(bDopeSheet *ads, const char *name)
{
  if (ads->flag & ADS_FLAG_FUZZY_NAMES) {
    /* full fuzzy, multi-word, case insensitive matches */
    const size_t str_len = strlen(ads->searchstr);
    const int words_max = BLI_string_max_possible_word_count(str_len);

    int(*words)[2] = static_cast<int(*)[2]>(BLI_array_alloca(words, words_max));
    const int words_len = BLI_string_find_split_words(
        ads->searchstr, str_len, ' ', words, words_max);
    bool found = false;

    /* match name against all search words */
    for (int index = 0; index < words_len; index++) {
      if (BLI_strncasestr(name, ads->searchstr + words[index][0], words[index][1])) {
        found = true;
        break;
      }
    }

    /* if we have a match somewhere, this returns true */
    return ((ads->flag & ADS_FLAG_INVERT_FILTER) == 0) ? found : !found;
  }
  /* fallback/default - just case insensitive, but starts from start of word */
  bool found = BLI_strcasestr(name, ads->searchstr) != nullptr;
  return ((ads->flag & ADS_FLAG_INVERT_FILTER) == 0) ? found : !found;
}

/* (Display-)Name-based F-Curve filtering
 * NOTE: when this function returns true, the F-Curve is to be skipped
 */
static bool skip_fcurve_with_name(
    bDopeSheet *ads, FCurve *fcu, eAnim_ChannelType channel_type, void *owner, ID *owner_id)
{
  bAnimListElem ale_dummy = {nullptr};
  const bAnimChannelType *acf;

  /* create a dummy wrapper for the F-Curve, so we can get typeinfo for it */
  ale_dummy.type = channel_type;
  ale_dummy.owner = owner;
  ale_dummy.id = owner_id;
  ale_dummy.data = fcu;

  /* get type info for channel */
  acf = ANIM_channel_get_typeinfo(&ale_dummy);
  if (acf && acf->name) {
    char name[256]; /* hopefully this will be enough! */

    /* get name */
    acf->name(&ale_dummy, name);

    /* check for partial match with the match string, assuming case insensitive filtering
     * if match, this channel shouldn't be ignored!
     */
    return !name_matches_dopesheet_filter(ads, name);
  }

  /* just let this go... */
  return true;
}

/**
 * Check if F-Curve has errors and/or is disabled
 *
 * \return true if F-Curve has errors/is disabled
 */
static bool fcurve_has_errors(const FCurve *fcu, bDopeSheet *ads)
{
  /* F-Curve disabled (path evaluation error). */
  if (fcu->flag & FCURVE_DISABLED) {
    return true;
  }

  /* driver? */
  if (fcu->driver) {
    const ChannelDriver *driver = fcu->driver;

    /* error flag on driver usually means that there is an error
     * BUT this may not hold with PyDrivers as this flag gets cleared
     *     if no critical errors prevent the driver from working...
     */
    if (driver->flag & DRIVER_FLAG_INVALID) {
      return true;
    }

    /* check variables for other things that need linting... */
    /* TODO: maybe it would be more efficient just to have a quick flag for this? */
    LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
      DRIVER_TARGETS_USED_LOOPER_BEGIN (dvar) {
        if (dtar->flag & DTAR_FLAG_INVALID) {
          return true;
        }

        if ((dtar->flag & DTAR_FLAG_FALLBACK_USED) &&
            (ads->filterflag2 & ADS_FILTER_DRIVER_FALLBACK_AS_ERROR))
        {
          return true;
        }
      }
      DRIVER_TARGETS_LOOPER_END;
    }
  }

  /* no errors found */
  return false;
}

/* find the next F-Curve that is usable for inclusion */
static FCurve *animfilter_fcurve_next(bDopeSheet *ads,
                                      FCurve *first,
                                      eAnim_ChannelType channel_type,
                                      int filter_mode,
                                      void *owner,
                                      ID *owner_id)
{
  bActionGroup *grp = (channel_type == ANIMTYPE_FCURVE) ? static_cast<bActionGroup *>(owner) :
                                                          nullptr;
  FCurve *fcu = nullptr;

  /* Loop over F-Curves - assume that the caller of this has already checked
   * that these should be included.
   * NOTE: we need to check if the F-Curves belong to the same group,
   * as this gets called for groups too...
   */
  for (fcu = first; ((fcu) && (fcu->grp == grp)); fcu = fcu->next) {
    /* special exception for Pose-Channel/Sequence-Strip/Node Based F-Curves:
     * - The 'Only Selected' and 'Include Hidden' data filters should be applied to sub-ID data
     *   which can be independently selected/hidden, such as Pose-Channels, Sequence Strips,
     *   and Nodes. Since these checks were traditionally done as first check for objects,
     *   we do the same here.
     * - We currently use an 'approximate' method for getting these F-Curves that doesn't require
     *   carefully checking the entire path.
     * - This will also affect things like Drivers, and also works for Bone Constraints.
     */
    if (ads && owner_id) {
      if ((filter_mode & ANIMFILTER_TMP_IGNORE_ONLYSEL) == 0) {
        if ((ads->filterflag & ADS_FILTER_ONLYSEL) ||
            (ads->filterflag & ADS_FILTER_INCL_HIDDEN) == 0)
        {
          if (skip_fcurve_selected_data(ads, fcu, owner_id, filter_mode)) {
            continue;
          }
        }
      }
    }

    /* only include if visible (Graph Editor check, not channels check) */
    if (!(filter_mode & ANIMFILTER_CURVE_VISIBLE) || (fcu->flag & FCURVE_VISIBLE)) {
      /* only work with this channel and its subchannels if it is editable */
      if (!(filter_mode & ANIMFILTER_FOREDIT) || EDITABLE_FCU(fcu)) {
        /* Only include this curve if selected in a way consistent
         * with the filtering requirements. */
        if (ANIMCHANNEL_SELOK(SEL_FCU(fcu)) && ANIMCHANNEL_SELEDITOK(SEL_FCU(fcu))) {
          /* only include if this curve is active */
          if (!(filter_mode & ANIMFILTER_ACTIVE) || (fcu->flag & FCURVE_ACTIVE)) {
            /* name based filtering... */
            if (((ads) && (ads->searchstr[0] != '\0')) && (owner_id)) {
              if (skip_fcurve_with_name(ads, fcu, channel_type, owner, owner_id)) {
                continue;
              }
            }

            /* error-based filtering... */
            if ((ads) && (ads->filterflag & ADS_FILTER_ONLY_ERRORS)) {
              /* skip if no errors... */
              if (!fcurve_has_errors(fcu, ads)) {
                continue;
              }
            }

            /* this F-Curve can be used, so return it */
            return fcu;
          }
        }
      }
    }
  }

  /* no (more) F-Curves from the list are suitable... */
  return nullptr;
}

static size_t animfilter_fcurves(ListBase *anim_data,
                                 bDopeSheet *ads,
                                 FCurve *first,
                                 eAnim_ChannelType fcurve_type,
                                 int filter_mode,
                                 void *owner,
                                 ID *owner_id,
                                 ID *fcurve_owner_id)
{
  FCurve *fcu;
  size_t items = 0;

  /* Loop over every F-Curve able to be included.
   *
   * This for-loop works like this:
   * 1) The starting F-Curve is assigned to the fcu pointer
   *    so that we have a starting point to search from.
   * 2) The first valid F-Curve to start from (which may include the one given as 'first')
   *    in the remaining list of F-Curves is found, and verified to be non-null.
   * 3) The F-Curve referenced by fcu pointer is added to the list
   * 4) The fcu pointer is set to the F-Curve after the one we just added,
   *    so that we can keep going through the rest of the F-Curve list without an eternal loop.
   *    Back to step 2 :)
   */
  for (fcu = first;
       (fcu = animfilter_fcurve_next(ads, fcu, fcurve_type, filter_mode, owner, owner_id));
       fcu = fcu->next)
  {
    if (UNLIKELY(fcurve_type == ANIMTYPE_NLACURVE)) {
      /* NLA Control Curve - Basically the same as normal F-Curves,
       * except we need to set some stuff differently */
      ANIMCHANNEL_NEW_CHANNEL_FULL(fcu, ANIMTYPE_NLACURVE, owner_id, fcurve_owner_id, {
        ale->owner = owner; /* strip */
        ale->adt = nullptr; /* to prevent time mapping from causing problems */
      });
    }
    else {
      /* Normal FCurve */
      ANIMCHANNEL_NEW_CHANNEL(fcu, ANIMTYPE_FCURVE, owner_id, fcurve_owner_id);
    }
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animfilter_act_group(bAnimContext *ac,
                                   ListBase *anim_data,
                                   bDopeSheet *ads,
                                   bAction *act,
                                   bActionGroup *agrp,
                                   int filter_mode,
                                   ID *owner_id)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;
  // int ofilter = filter_mode;

  /* if we care about the selection status of the channels,
   * but the group isn't expanded (1)...
   * (1) this only matters if we actually care about the hierarchy though.
   *     - Hierarchy matters: this hack should be applied
   *     - Hierarchy ignored: cases like #21276 won't work properly, unless we skip this hack
   */
  if (
      /* Care about hierarchy but group isn't expanded. */
      ((filter_mode & ANIMFILTER_LIST_VISIBLE) && EXPANDED_AGRP(ac, agrp) == 0) &&
      /* Care about selection status. */
      (filter_mode & (ANIMFILTER_SEL | ANIMFILTER_UNSEL)))
  {
    /* If the group itself isn't selected appropriately,
     * we shouldn't consider its children either. */
    if (ANIMCHANNEL_SELOK(SEL_AGRP(agrp)) == 0) {
      return 0;
    }

    /* if we're still here,
     * then the selection status of the curves within this group should not matter,
     * since this creates too much overhead for animators (i.e. making a slow workflow).
     *
     * Tools affected by this at time of coding (2010 Feb 09):
     * - Inserting keyframes on selected channels only.
     * - Pasting keyframes.
     * - Creating ghost curves in Graph Editor.
     */
    filter_mode &= ~(ANIMFILTER_SEL | ANIMFILTER_UNSEL | ANIMFILTER_LIST_VISIBLE);
  }

  /* add grouped F-Curves */
  BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_AGRP(ac, agrp)) {
    /* special filter so that we can get just the F-Curves within the active group */
    if (!(filter_mode & ANIMFILTER_ACTGROUPED) || (agrp->flag & AGRP_ACTIVE)) {
      /* for the Graph Editor, curves may be set to not be visible in the view to lessen
       * clutter, but to do this, we need to check that the group doesn't have its
       * not-visible flag set preventing all its sub-curves to be shown
       */
      if (!(filter_mode & ANIMFILTER_CURVE_VISIBLE) || !(agrp->flag & AGRP_NOTVISIBLE)) {
        /* group must be editable for its children to be editable (if we care about this) */
        if (!(filter_mode & ANIMFILTER_FOREDIT) || EDITABLE_AGRP(agrp)) {
          /* get first F-Curve which can be used here */
          FCurve *first_fcu = animfilter_fcurve_next(ads,
                                                     static_cast<FCurve *>(agrp->channels.first),
                                                     ANIMTYPE_FCURVE,
                                                     filter_mode,
                                                     agrp,
                                                     owner_id);

          /* filter list, starting from this F-Curve */
          tmp_items += animfilter_fcurves(
              &tmp_data, ads, first_fcu, ANIMTYPE_FCURVE, filter_mode, agrp, owner_id, &act->id);
        }
      }
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* add this group as a channel first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* restore original filter mode so that this next step works ok... */
      // filter_mode = ofilter;

      /* filter selection of channel specially here again,
       * since may be open and not subject to previous test */
      if (ANIMCHANNEL_SELOK(SEL_AGRP(agrp))) {
        ANIMCHANNEL_NEW_CHANNEL(agrp, ANIMTYPE_GROUP, owner_id, &act->id);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animfilter_action(bAnimContext *ac,
                                ListBase *anim_data,
                                bDopeSheet *ads,
                                bAction *act,
                                int filter_mode,
                                ID *owner_id)
{
  FCurve *lastchan = nullptr;
  size_t items = 0;

  /* don't include anything from this action if it is linked in from another file,
   * and we're getting stuff for editing...
   */
  if ((filter_mode & ANIMFILTER_FOREDIT) && (ID_IS_LINKED(act) || ID_IS_OVERRIDE_LIBRARY(act))) {
    return 0;
  }

  /* do groups */
  /* TODO: do nested groups? */
  LISTBASE_FOREACH (bActionGroup *, agrp, &act->groups) {
    /* store reference to last channel of group */
    if (agrp->channels.last) {
      lastchan = static_cast<FCurve *>(agrp->channels.last);
    }

    /* action group's channels */
    items += animfilter_act_group(ac, anim_data, ads, act, agrp, filter_mode, owner_id);
  }

  /* un-grouped F-Curves (only if we're not only considering those channels in the active group) */
  if (!(filter_mode & ANIMFILTER_ACTGROUPED)) {
    FCurve *firstfcu = (lastchan) ? (lastchan->next) : static_cast<FCurve *>(act->curves.first);
    items += animfilter_fcurves(
        anim_data, ads, firstfcu, ANIMTYPE_FCURVE, filter_mode, nullptr, owner_id, &act->id);
  }

  /* return the number of items added to the list */
  return items;
}

/* Include NLA-Data for NLA-Editor:
 * - When ANIMFILTER_LIST_CHANNELS is used, that means we should be filtering the list for display
 *   Although the evaluation order is from the first track to the last and then apply the
 *   Action on top, we present this in the UI as the Active Action followed by the last track
 *   to the first so that we get the evaluation order presented as per a stack.
 * - For normal filtering (i.e. for editing),
 *   we only need the NLA-tracks but they can be in 'normal' evaluation order, i.e. first to last.
 *   Otherwise, some tools may get screwed up.
 */
static size_t animfilter_nla(bAnimContext * /*ac*/,
                             ListBase *anim_data,
                             bDopeSheet *ads,
                             AnimData *adt,
                             int filter_mode,
                             ID *owner_id)
{
  NlaTrack *nlt;
  NlaTrack *first = nullptr, *next = nullptr;
  size_t items = 0;

  /* if showing channels, include active action */
  if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
    /* if NLA action-line filtering is off, don't show unless there are keyframes,
     * in order to keep things more compact for doing transforms
     */
    if (!(ads->filterflag & ADS_FILTER_NLA_NOACT) || (adt->action)) {
      /* there isn't really anything editable here, so skip if need editable */
      if ((filter_mode & ANIMFILTER_FOREDIT) == 0) {
        /* Just add the action track now (this MUST appear for drawing):
         * - As AnimData may not have an action,
         *   we pass a dummy pointer just to get the list elem created,
         *   then overwrite this with the real value - REVIEW THIS.
         */
        ANIMCHANNEL_NEW_CHANNEL_FULL(
            (void *)(&adt->action), ANIMTYPE_NLAACTION, owner_id, nullptr, {
              ale->data = adt->action ? adt->action : nullptr;
            });
      }
    }

    /* first track to include will be the last one if we're filtering by channels */
    first = static_cast<NlaTrack *>(adt->nla_tracks.last);
  }
  else {
    /* first track to include will the first one (as per normal) */
    first = static_cast<NlaTrack *>(adt->nla_tracks.first);
  }

  /* loop over NLA Tracks -
   * assume that the caller of this has already checked that these should be included */
  for (nlt = first; nlt; nlt = next) {
    /* 'next' NLA-Track to use depends on whether we're filtering for drawing or not */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      next = nlt->prev;
    }
    else {
      next = nlt->next;
    }

    /* only work with this channel and its subchannels if it is editable */
    if (!(filter_mode & ANIMFILTER_FOREDIT) || EDITABLE_NLT(nlt)) {
      /* only include this track if selected in a way consistent with the filtering requirements */
      if (ANIMCHANNEL_SELOK(SEL_NLT(nlt))) {
        /* only include if this track is active */
        if (!(filter_mode & ANIMFILTER_ACTIVE) || (nlt->flag & NLATRACK_ACTIVE)) {
          /* name based filtering... */
          if (((ads) && (ads->searchstr[0] != '\0')) && (owner_id)) {
            bool track_ok = false, strip_ok = false;

            /* check if the name of the track, or the strips it has are ok... */
            track_ok = name_matches_dopesheet_filter(ads, nlt->name);

            if (track_ok == false) {
              LISTBASE_FOREACH (NlaStrip *, strip, &nlt->strips) {
                if (name_matches_dopesheet_filter(ads, strip->name)) {
                  strip_ok = true;
                  break;
                }
              }
            }

            /* skip if both fail this test... */
            if (!track_ok && !strip_ok) {
              continue;
            }
          }

          /* add the track now that it has passed all our tests */
          ANIMCHANNEL_NEW_CHANNEL(nlt, ANIMTYPE_NLATRACK, owner_id, nullptr);
        }
      }
    }
  }

  /* return the number of items added to the list */
  return items;
}

/* Include the control FCurves per NLA Strip in the channel list
 * NOTE: This is includes the expander too...
 */
static size_t animfilter_nla_controls(
    ListBase *anim_data, bDopeSheet *ads, AnimData *adt, int filter_mode, ID *owner_id)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add control curves from each NLA strip... */
  /* NOTE: ANIMTYPE_FCURVES are created here, to avoid duplicating the code needed */
  BEGIN_ANIMFILTER_SUBCHANNELS ((adt->flag & ADT_NLA_SKEYS_COLLAPSED) == 0) {
    /* for now, we only go one level deep - so controls on grouped FCurves are not handled */
    LISTBASE_FOREACH (NlaTrack *, nlt, &adt->nla_tracks) {
      LISTBASE_FOREACH (NlaStrip *, strip, &nlt->strips) {
        /* pass strip as the "owner",
         * so that the name lookups (used while filtering) will resolve */
        /* NLA tracks are coming from AnimData, so owner of f-curves
         * is the same as owner of animation data. */
        tmp_items += animfilter_fcurves(&tmp_data,
                                        ads,
                                        static_cast<FCurve *>(strip->fcurves.first),
                                        ANIMTYPE_NLACURVE,
                                        filter_mode,
                                        strip,
                                        owner_id,
                                        owner_id);
      }
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* add the expander as a channel first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* currently these channels cannot be selected, so they should be skipped */
      if ((filter_mode & (ANIMFILTER_SEL | ANIMFILTER_UNSEL)) == 0) {
        ANIMCHANNEL_NEW_CHANNEL(adt, ANIMTYPE_NLACONTROLS, owner_id, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* determine what animation data from AnimData block should get displayed */
static size_t animfilter_block_data(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, ID *id, int filter_mode)
{
  AnimData *adt = BKE_animdata_from_id(id);
  size_t items = 0;

  /* image object data-blocks have no anim-data so check for nullptr */
  if (adt) {
    IdAdtTemplate *iat = (IdAdtTemplate *)id;

    /* NOTE: this macro is used instead of inlining the logic here,
     * since this sort of filtering is still needed in a few places in the rest of the code still -
     * notably for the few cases where special mode-based
     * different types of data expanders are required.
     */
    ANIMDATA_FILTER_CASES(
        iat,
        { /* AnimData */
          /* specifically filter animdata block */
          if (ANIMCHANNEL_SELOK(SEL_ANIMDATA(adt))) {
            ANIMCHANNEL_NEW_CHANNEL(adt, ANIMTYPE_ANIMDATA, id, nullptr);
          }
        },
        { /* NLA */
          items += animfilter_nla(ac, anim_data, ads, adt, filter_mode, id);
        },
        { /* Drivers */
          items += animfilter_fcurves(anim_data,
                                      ads,
                                      static_cast<FCurve *>(adt->drivers.first),
                                      ANIMTYPE_FCURVE,
                                      filter_mode,
                                      nullptr,
                                      id,
                                      id);
        },
        { /* NLA Control Keyframes */
          items += animfilter_nla_controls(anim_data, ads, adt, filter_mode, id);
        },
        { /* Keyframes */
          items += animfilter_action(ac, anim_data, ads, adt->action, filter_mode, id);
        });
  }

  return items;
}

/* Include ShapeKey Data for ShapeKey Editor */
static size_t animdata_filter_shapekey(bAnimContext *ac,
                                       ListBase *anim_data,
                                       Key *key,
                                       int filter_mode)
{
  size_t items = 0;

  /* check if channels or only F-Curves */
  if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
    bDopeSheet *ads = ac->ads;

    /* loop through the channels adding ShapeKeys as appropriate */
    LISTBASE_FOREACH (KeyBlock *, kb, &key->block) {
      /* skip the first one, since that's the non-animatable basis */
      if (kb == key->block.first) {
        continue;
      }

      /* Skip shapekey if the name doesn't match the filter string. */
      if (ads != nullptr && ads->searchstr[0] != '\0' &&
          name_matches_dopesheet_filter(ads, kb->name) == false)
      {
        continue;
      }

      /* only work with this channel and its subchannels if it is editable */
      if (!(filter_mode & ANIMFILTER_FOREDIT) || EDITABLE_SHAPEKEY(kb)) {
        /* Only include this track if selected in a way consistent
         * with the filtering requirements. */
        if (ANIMCHANNEL_SELOK(SEL_SHAPEKEY(kb))) {
          /* TODO: consider 'active' too? */

          /* owner-id here must be key so that the F-Curve can be resolved... */
          ANIMCHANNEL_NEW_CHANNEL(kb, ANIMTYPE_SHAPEKEY, key, nullptr);
        }
      }
    }
  }
  else {
    /* just use the action associated with the shapekey */
    /* TODO: somehow manage to pass dopesheet info down here too? */
    if (key->adt) {
      if (filter_mode & ANIMFILTER_ANIMDATA) {
        if (ANIMCHANNEL_SELOK(SEL_ANIMDATA(key->adt))) {
          ANIMCHANNEL_NEW_CHANNEL(key->adt, ANIMTYPE_ANIMDATA, key, nullptr);
        }
      }
      else if (key->adt->action) {
        items = animfilter_action(
            ac, anim_data, nullptr, key->adt->action, filter_mode, (ID *)key);
      }
    }
  }

  /* return the number of items added to the list */
  return items;
}

/* Helper for Grease Pencil - layers within a data-block. */

static size_t animdata_filter_grease_pencil_layer(ListBase *anim_data,
                                                  bDopeSheet * /*ads*/,
                                                  GreasePencil *grease_pencil,
                                                  blender::bke::greasepencil::Layer &layer,
                                                  int filter_mode)
{

  size_t items = 0;

  /* Only if the layer is selected. */
  if (!ANIMCHANNEL_SELOK(layer.is_selected())) {
    return items;
  }

  /* Only if the layer is editable. */
  if ((filter_mode & ANIMFILTER_FOREDIT) && layer.is_locked()) {
    return items;
  }

  /* Only if the layer is active. */
  if ((filter_mode & ANIMFILTER_ACTIVE) && grease_pencil->is_layer_active(&layer)) {
    return items;
  }

  /* Skip empty layers. */
  if (layer.is_empty()) {
    return items;
  }

  /* Add layer channel. */
  ANIMCHANNEL_NEW_CHANNEL(
      static_cast<void *>(&layer), ANIMTYPE_GREASE_PENCIL_LAYER, grease_pencil, nullptr);

  return items;
}

static size_t animdata_filter_grease_pencil_layer_node_recursive(
    ListBase *anim_data,
    bDopeSheet *ads,
    GreasePencil *grease_pencil,
    blender::bke::greasepencil::TreeNode &node,
    int filter_mode)
{
  using namespace blender::bke::greasepencil;
  size_t items = 0;

  /* Skip node if the name doesn't match the filter string. */
  const bool name_search = (ads->searchstr[0] != '\0');
  const bool skip_node = name_search && !name_matches_dopesheet_filter(ads, node.name().c_str());

  if (node.is_layer() && !skip_node) {
    items += animdata_filter_grease_pencil_layer(
        anim_data, ads, grease_pencil, node.as_layer(), filter_mode);
  }
  else if (node.is_group()) {
    const LayerGroup &layer_group = node.as_group();

    ListBase tmp_data = {nullptr, nullptr};
    size_t tmp_items = 0;

    /* Add grease pencil layer channels. */
    BEGIN_ANIMFILTER_SUBCHANNELS (layer_group.base.flag &GP_LAYER_TREE_NODE_EXPANDED) {
      LISTBASE_FOREACH_BACKWARD (GreasePencilLayerTreeNode *, node_, &layer_group.children) {
        tmp_items += animdata_filter_grease_pencil_layer_node_recursive(
            &tmp_data, ads, grease_pencil, node_->wrap(), filter_mode);
      }
    }
    END_ANIMFILTER_SUBCHANNELS;

    if ((tmp_items == 0) && !name_search) {
      /* If no sub-channels, return early.
       * Except if the search by name is on, because we might want to display the layer group alone
       * in that case. */
      return items;
    }

    if ((filter_mode & ANIMFILTER_LIST_CHANNELS) && !skip_node) {
      /* Add data block container (if for drawing, and it contains sub-channels). */
      ANIMCHANNEL_NEW_CHANNEL(
          static_cast<void *>(&node), ANIMTYPE_GREASE_PENCIL_LAYER_GROUP, grease_pencil, nullptr);
    }

    /* Add the list of collected channels. */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }
  return items;
}

static size_t animdata_filter_grease_pencil_layers_data(ListBase *anim_data,
                                                        bDopeSheet *ads,
                                                        GreasePencil *grease_pencil,
                                                        int filter_mode)
{
  size_t items = 0;

  LISTBASE_FOREACH_BACKWARD (
      GreasePencilLayerTreeNode *, node, &grease_pencil->root_group_ptr->children)
  {
    items += animdata_filter_grease_pencil_layer_node_recursive(
        anim_data, ads, grease_pencil, node->wrap(), filter_mode);
  }

  return items;
}

/* Helper for Grease Pencil - layers within a data-block. */
static size_t animdata_filter_gpencil_layers_data_legacy(ListBase *anim_data,
                                                         bDopeSheet *ads,
                                                         bGPdata *gpd,
                                                         int filter_mode)
{
  size_t items = 0;

  /* loop over layers as the conditions are acceptable (top-Down order) */
  LISTBASE_FOREACH_BACKWARD (bGPDlayer *, gpl, &gpd->layers) {
    /* only if selected */
    if (!ANIMCHANNEL_SELOK(SEL_GPL(gpl))) {
      continue;
    }

    /* only if editable */
    if ((filter_mode & ANIMFILTER_FOREDIT) && !EDITABLE_GPL(gpl)) {
      continue;
    }

    /* active... */
    if ((filter_mode & ANIMFILTER_ACTIVE) && (gpl->flag & GP_LAYER_ACTIVE) == 0) {
      continue;
    }

    /* skip layer if the name doesn't match the filter string */
    if (ads != nullptr && ads->searchstr[0] != '\0' &&
        name_matches_dopesheet_filter(ads, gpl->info) == false)
    {
      continue;
    }

    /* Skip empty layers. */
    if (BLI_listbase_is_empty(&gpl->frames)) {
      continue;
    }

    /* add to list */
    ANIMCHANNEL_NEW_CHANNEL(gpl, ANIMTYPE_GPLAYER, gpd, nullptr);
  }

  return items;
}

static size_t animdata_filter_grease_pencil_data(ListBase *anim_data,
                                                 bDopeSheet *ads,
                                                 GreasePencil *grease_pencil,
                                                 int filter_mode)
{
  using namespace blender;

  size_t items = 0;

  /* When asked from "AnimData" blocks (i.e. the top-level containers for normal animation),
   * for convenience, this will return grease pencil data-blocks instead.
   * This may cause issues down the track, but for now, this will do.
   */
  if (filter_mode & ANIMFILTER_ANIMDATA) {
    /* Just add data block container. */
    ANIMCHANNEL_NEW_CHANNEL(
        grease_pencil, ANIMTYPE_GREASE_PENCIL_DATABLOCK, grease_pencil, nullptr);
  }
  else {
    ListBase tmp_data = {nullptr, nullptr};
    size_t tmp_items = 0;

    if (!(filter_mode & ANIMFILTER_FCURVESONLY)) {
      /* Add grease pencil layer channels. */
      BEGIN_ANIMFILTER_SUBCHANNELS (grease_pencil->flag &GREASE_PENCIL_ANIM_CHANNEL_EXPANDED) {
        tmp_items += animdata_filter_grease_pencil_layers_data(
            &tmp_data, ads, grease_pencil, filter_mode);
      }
      END_ANIMFILTER_SUBCHANNELS;
    }

    if (tmp_items == 0) {
      /* If no sub-channels, return early. */
      return items;
    }

    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* Add data block container (if for drawing, and it contains sub-channels). */
      ANIMCHANNEL_NEW_CHANNEL(
          grease_pencil, ANIMTYPE_GREASE_PENCIL_DATABLOCK, grease_pencil, nullptr);
    }

    /* Add the list of collected channels. */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  return items;
}

/* Helper for Grease Pencil - Grease Pencil data-block - GP Frames. */
static size_t animdata_filter_gpencil_legacy_data(ListBase *anim_data,
                                                  bDopeSheet *ads,
                                                  bGPdata *gpd,
                                                  int filter_mode)
{
  size_t items = 0;

  /* When asked from "AnimData" blocks (i.e. the top-level containers for normal animation),
   * for convenience, this will return GP Data-blocks instead.
   * This may cause issues down the track, but for now, this will do.
   */
  if (filter_mode & ANIMFILTER_ANIMDATA) {
    /* just add GPD as a channel - this will add everything needed */
    ANIMCHANNEL_NEW_CHANNEL(gpd, ANIMTYPE_GPDATABLOCK, gpd, nullptr);
  }
  else {
    ListBase tmp_data = {nullptr, nullptr};
    size_t tmp_items = 0;

    if (!(filter_mode & ANIMFILTER_FCURVESONLY)) {
      /* add gpencil animation channels */
      BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_GPD(gpd)) {
        tmp_items += animdata_filter_gpencil_layers_data_legacy(&tmp_data, ads, gpd, filter_mode);
      }
      END_ANIMFILTER_SUBCHANNELS;
    }

    /* did we find anything? */
    if (tmp_items) {
      /* include data-expand widget first */
      if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
        /* add gpd as channel too (if for drawing, and it has layers) */
        ANIMCHANNEL_NEW_CHANNEL(gpd, ANIMTYPE_GPDATABLOCK, nullptr, nullptr);
      }

      /* now add the list of collected channels */
      BLI_movelisttolist(anim_data, &tmp_data);
      BLI_assert(BLI_listbase_is_empty(&tmp_data));
      items += tmp_items;
    }
  }

  return items;
}

static size_t animdata_filter_grease_pencil(bAnimContext *ac, ListBase *anim_data, int filter_mode)
{
  size_t items = 0;
  Scene *scene = ac->scene;
  ViewLayer *view_layer = (ViewLayer *)ac->view_layer;
  bDopeSheet *ads = ac->ads;

  BKE_view_layer_synced_ensure(scene, view_layer);
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    if (!base->object || (base->object->type != OB_GREASE_PENCIL)) {
      continue;
    }
    Object *ob = base->object;

    if ((filter_mode & ANIMFILTER_DATA_VISIBLE) && !(ads->filterflag & ADS_FILTER_INCL_HIDDEN)) {
      /* Layer visibility - we check both object and base,
       * since these may not be in sync yet. */
      if ((base->flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0 ||
          (base->flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT) == 0)
      {
        continue;
      }

      /* Outliner restrict-flag */
      if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
        continue;
      }
    }

    /* Check selection and object type filters */
    if ((ads->filterflag & ADS_FILTER_ONLYSEL) && !(base->flag & BASE_SELECTED)) {
      /* Only selected should be shown */
      continue;
    }

    if (ads->filter_grp != nullptr) {
      if (BKE_collection_has_object_recursive(ads->filter_grp, ob) == 0) {
        continue;
      }
    }

    items += animdata_filter_grease_pencil_data(
        anim_data, ads, static_cast<GreasePencil *>(ob->data), filter_mode);
  }

  /* Return the number of items added to the list */
  return items;
}

/**
 * Grab all Grease Pencil data-blocks in file.
 *
 * TODO: should this be amalgamated with the dope-sheet filtering code?
 */
static size_t animdata_filter_gpencil_legacy(bAnimContext *ac,
                                             ListBase *anim_data,
                                             void * /*data*/,
                                             int filter_mode)
{
  bDopeSheet *ads = ac->ads;
  size_t items = 0;

  Scene *scene = ac->scene;
  ViewLayer *view_layer = (ViewLayer *)ac->view_layer;

  /* Include all annotation datablocks. */
  if (((ads->filterflag & ADS_FILTER_ONLYSEL) == 0) || (ads->filterflag & ADS_FILTER_INCL_HIDDEN))
  {
    LISTBASE_FOREACH (bGPdata *, gpd, &ac->bmain->gpencils) {
      if (gpd->flag & GP_DATA_ANNOTATIONS) {
        items += animdata_filter_gpencil_legacy_data(anim_data, ads, gpd, filter_mode);
      }
    }
  }
  /* Objects in the scene */
  BKE_view_layer_synced_ensure(scene, view_layer);
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    /* Only consider this object if it has got some GP data (saving on all the other tests) */
    if (base->object && (base->object->type == OB_GPENCIL_LEGACY)) {
      Object *ob = base->object;

      /* firstly, check if object can be included, by the following factors:
       * - if only visible, must check for layer and also viewport visibility
       *   --> while tools may demand only visible, user setting takes priority
       *       as user option controls whether sets of channels get included while
       *       tool-flag takes into account collapsed/open channels too
       * - if only selected, must check if object is selected
       * - there must be animation data to edit (this is done recursively as we
       *   try to add the channels)
       */
      if ((filter_mode & ANIMFILTER_DATA_VISIBLE) && !(ads->filterflag & ADS_FILTER_INCL_HIDDEN)) {
        /* Layer visibility - we check both object and base,
         * since these may not be in sync yet. */
        if ((base->flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0 ||
            (base->flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT) == 0)
        {
          continue;
        }

        /* outliner restrict-flag */
        if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
          continue;
        }
      }

      /* check selection and object type filters */
      if ((ads->filterflag & ADS_FILTER_ONLYSEL) && !(base->flag & BASE_SELECTED)) {
        /* only selected should be shown */
        continue;
      }

      /* check if object belongs to the filtering group if option to filter
       * objects by the grouped status is on
       * - used to ease the process of doing multiple-character choreographies
       */
      if (ads->filter_grp != nullptr) {
        if (BKE_collection_has_object_recursive(ads->filter_grp, ob) == 0) {
          continue;
        }
      }

      /* finally, include this object's grease pencil data-block. */
      /* XXX: Should we store these under expanders per item? */
      items += animdata_filter_gpencil_legacy_data(
          anim_data, ads, static_cast<bGPdata *>(ob->data), filter_mode);
    }
  }

  /* return the number of items added to the list */
  return items;
}

/* Helper for Grease Pencil data integrated with main DopeSheet */
static size_t animdata_filter_ds_gpencil(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, bGPdata *gpd, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add relevant animation channels for Grease Pencil */
  BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_GPD(gpd)) {
    /* add animation channels */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, &gpd->id, filter_mode);

    /* add Grease Pencil layers */
    if (!(filter_mode & ANIMFILTER_FCURVESONLY)) {
      tmp_items += animdata_filter_gpencil_layers_data_legacy(&tmp_data, ads, gpd, filter_mode);
    }

    /* TODO: do these need a separate expander?
     * XXX:  what order should these go in? */
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      /* XXX: active check here needs checking */
      if (ANIMCHANNEL_ACTIVEOK(gpd)) {
        ANIMCHANNEL_NEW_CHANNEL(gpd, ANIMTYPE_DSGPENCIL, gpd, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* Helper for Cache File data integrated with main DopeSheet */
static size_t animdata_filter_ds_cachefile(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, CacheFile *cache_file, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add relevant animation channels for Cache File */
  BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_CACHEFILE_OBJD(cache_file)) {
    /* add animation channels */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, &cache_file->id, filter_mode);
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      /* XXX: active check here needs checking */
      if (ANIMCHANNEL_ACTIVEOK(cache_file)) {
        ANIMCHANNEL_NEW_CHANNEL(cache_file, ANIMTYPE_DSCACHEFILE, cache_file, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* Helper for Mask Editing - mask layers */
static size_t animdata_filter_mask_data(ListBase *anim_data, Mask *mask, const int filter_mode)
{
  const MaskLayer *masklay_act = BKE_mask_layer_active(mask);
  size_t items = 0;

  LISTBASE_FOREACH (MaskLayer *, masklay, &mask->masklayers) {
    if (!ANIMCHANNEL_SELOK(SEL_MASKLAY(masklay))) {
      continue;
    }

    if ((filter_mode & ANIMFILTER_FOREDIT) && !EDITABLE_MASK(masklay)) {
      continue;
    }

    if ((filter_mode & ANIMFILTER_ACTIVE) & (masklay_act != masklay)) {
      continue;
    }

    ANIMCHANNEL_NEW_CHANNEL(masklay, ANIMTYPE_MASKLAYER, mask, nullptr);
  }

  return items;
}

/* Grab all mask data */
static size_t animdata_filter_mask(Main *bmain,
                                   ListBase *anim_data,
                                   void * /*data*/,
                                   int filter_mode)
{
  size_t items = 0;

  /* For now, grab mask data-blocks directly from main. */
  /* XXX: this is not good... */
  LISTBASE_FOREACH (Mask *, mask, &bmain->masks) {
    ListBase tmp_data = {nullptr, nullptr};
    size_t tmp_items = 0;

    /* only show if mask is used by something... */
    if (ID_REAL_USERS(mask) < 1) {
      continue;
    }

    /* add mask animation channels */
    if (!(filter_mode & ANIMFILTER_FCURVESONLY)) {
      BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_MASK(mask)) {
        tmp_items += animdata_filter_mask_data(&tmp_data, mask, filter_mode);
      }
      END_ANIMFILTER_SUBCHANNELS;
    }

    /* did we find anything? */
    if (!tmp_items) {
      continue;
    }

    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* add mask data-block as channel too (if for drawing, and it has layers) */
      ANIMCHANNEL_NEW_CHANNEL(mask, ANIMTYPE_MASKDATABLOCK, nullptr, nullptr);
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* NOTE: owner_id is scene, material, or texture block,
 * which is the direct owner of the node tree in question. */
static size_t animdata_filter_ds_nodetree_group(bAnimContext *ac,
                                                ListBase *anim_data,
                                                bDopeSheet *ads,
                                                ID *owner_id,
                                                bNodeTree *ntree,
                                                int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add nodetree animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_NTREE_DATA(ntree)) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)ntree, filter_mode);
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      if (ANIMCHANNEL_ACTIVEOK(ntree)) {
        ANIMCHANNEL_NEW_CHANNEL(ntree, ANIMTYPE_DSNTREE, owner_id, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_ds_nodetree(bAnimContext *ac,
                                          ListBase *anim_data,
                                          bDopeSheet *ads,
                                          ID *owner_id,
                                          bNodeTree *ntree,
                                          int filter_mode)
{
  size_t items = 0;

  items += animdata_filter_ds_nodetree_group(ac, anim_data, ads, owner_id, ntree, filter_mode);

  LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
    if (node->type == NODE_GROUP) {
      if (node->id) {
        if ((ads->filterflag & ADS_FILTER_ONLYSEL) && (node->flag & NODE_SELECT) == 0) {
          continue;
        }
        /* Recurse into the node group */
        items += animdata_filter_ds_nodetree(ac,
                                             anim_data,
                                             ads,
                                             owner_id,
                                             (bNodeTree *)node->id,
                                             filter_mode | ANIMFILTER_TMP_IGNORE_ONLYSEL);
      }
    }
  }

  return items;
}

static size_t animdata_filter_ds_linestyle(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Scene *sce, int filter_mode)
{
  size_t items = 0;

  LISTBASE_FOREACH (ViewLayer *, view_layer, &sce->view_layers) {
    LISTBASE_FOREACH (FreestyleLineSet *, lineset, &view_layer->freestyle_config.linesets) {
      if (lineset->linestyle) {
        lineset->linestyle->id.tag |= LIB_TAG_DOIT;
      }
    }
  }

  LISTBASE_FOREACH (ViewLayer *, view_layer, &sce->view_layers) {
    /* skip render layers without Freestyle enabled */
    if ((view_layer->flag & VIEW_LAYER_FREESTYLE) == 0) {
      continue;
    }

    /* loop over linesets defined in the render layer */
    LISTBASE_FOREACH (FreestyleLineSet *, lineset, &view_layer->freestyle_config.linesets) {
      FreestyleLineStyle *linestyle = lineset->linestyle;
      ListBase tmp_data = {nullptr, nullptr};
      size_t tmp_items = 0;

      if ((linestyle == nullptr) || !(linestyle->id.tag & LIB_TAG_DOIT)) {
        continue;
      }
      linestyle->id.tag &= ~LIB_TAG_DOIT;

      /* add scene-level animation channels */
      BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_LS_SCED(linestyle)) {
        /* animation data filtering */
        tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)linestyle, filter_mode);
      }
      END_ANIMFILTER_SUBCHANNELS;

      /* did we find anything? */
      if (tmp_items) {
        /* include anim-expand widget first */
        if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
          /* check if filtering by active status */
          if (ANIMCHANNEL_ACTIVEOK(linestyle)) {
            ANIMCHANNEL_NEW_CHANNEL(linestyle, ANIMTYPE_DSLINESTYLE, sce, nullptr);
          }
        }

        /* now add the list of collected channels */
        BLI_movelisttolist(anim_data, &tmp_data);
        BLI_assert(BLI_listbase_is_empty(&tmp_data));
        items += tmp_items;
      }
    }
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_ds_texture(bAnimContext *ac,
                                         ListBase *anim_data,
                                         bDopeSheet *ads,
                                         Tex *tex,
                                         ID *owner_id,
                                         int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add texture's animation data to temp collection */
  BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_TEX_DATA(tex)) {
    /* texture animdata */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)tex, filter_mode);

    /* nodes */
    if ((tex->nodetree) && !(ads->filterflag & ADS_FILTER_NONTREE)) {
      /* owner_id as id instead of texture,
       * since it'll otherwise be impossible to track the depth. */

      /* FIXME: perhaps as a result, textures should NOT be included under materials,
       * but under their own section instead so that free-floating textures can also be animated.
       */
      tmp_items += animdata_filter_ds_nodetree(
          ac, &tmp_data, ads, (ID *)tex, tex->nodetree, filter_mode);
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include texture-expand widget? */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      if (ANIMCHANNEL_ACTIVEOK(tex)) {
        ANIMCHANNEL_NEW_CHANNEL(tex, ANIMTYPE_DSTEX, owner_id, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* NOTE: owner_id is the direct owner of the texture stack in question
 *       It used to be Material/Light/World before the Blender Internal removal for 2.8
 */
static size_t animdata_filter_ds_textures(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, ID *owner_id, int filter_mode)
{
  MTex **mtex = nullptr;
  size_t items = 0;
  int a = 0;

  /* get datatype specific data first */
  if (owner_id == nullptr) {
    return 0;
  }

  switch (GS(owner_id->name)) {
    case ID_PA: {
      ParticleSettings *part = (ParticleSettings *)owner_id;
      mtex = (MTex **)(&part->mtex);
      break;
    }
    default: {
      /* invalid/unsupported option */
      if (G.debug & G_DEBUG) {
        printf("ERROR: Unsupported owner_id (i.e. texture stack) for filter textures - %s\n",
               owner_id->name);
      }
      return 0;
    }
  }

  /* Firstly check that we actually have some textures,
   * by gathering all textures in a temp list. */
  for (a = 0; a < MAX_MTEX; a++) {
    Tex *tex = (mtex[a]) ? mtex[a]->tex : nullptr;

    /* for now, if no texture returned, skip (this shouldn't confuse the user I hope) */
    if (tex == nullptr) {
      continue;
    }

    /* add texture's anim channels */
    items += animdata_filter_ds_texture(ac, anim_data, ads, tex, owner_id, filter_mode);
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_ds_material(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Material *ma, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add material's animation data to temp collection */
  BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_MAT_OBJD(ma)) {
    /* material's animation data */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)ma, filter_mode);

    /* nodes */
    if ((ma->nodetree) && !(ads->filterflag & ADS_FILTER_NONTREE)) {
      tmp_items += animdata_filter_ds_nodetree(
          ac, &tmp_data, ads, (ID *)ma, ma->nodetree, filter_mode);
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include material-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      if (ANIMCHANNEL_ACTIVEOK(ma)) {
        ANIMCHANNEL_NEW_CHANNEL(ma, ANIMTYPE_DSMAT, ma, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  return items;
}

static size_t animdata_filter_ds_materials(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Object *ob, int filter_mode)
{
  size_t items = 0;
  int a = 0;

  /* First pass: take the materials referenced via the Material slots of the object. */
  for (a = 1; a <= ob->totcol; a++) {
    Material *ma = BKE_object_material_get(ob, a);

    /* if material is valid, try to add relevant contents from here */
    if (ma) {
      /* add channels */
      items += animdata_filter_ds_material(ac, anim_data, ads, ma, filter_mode);
    }
  }

  /* return the number of items added to the list */
  return items;
}

/* ............ */

/* Temporary context for modifier linked-data channel extraction */
struct tAnimFilterModifiersContext {
  bAnimContext *ac; /* anim editor context */
  bDopeSheet *ads;  /* dopesheet filtering settings */

  ListBase tmp_data; /* list of channels created (but not yet added to the main list) */
  size_t items;      /* number of channels created */

  int filter_mode; /* flags for stuff we want to filter */
};

/* dependency walker callback for modifier dependencies */
static void animfilter_modifier_idpoin_cb(void *afm_ptr, Object *ob, ID **idpoin, int /*cb_flag*/)
{
  tAnimFilterModifiersContext *afm = (tAnimFilterModifiersContext *)afm_ptr;
  ID *owner_id = &ob->id;
  ID *id = *idpoin;

  /* NOTE: the walker only guarantees to give us all the ID-ptr *slots*,
   * not just the ones which are actually used, so be careful!
   */
  if (id == nullptr) {
    return;
  }

  /* check if this is something we're interested in... */
  switch (GS(id->name)) {
    case ID_TE: /* Textures */
    {
      Tex *tex = (Tex *)id;
      if (!(afm->ads->filterflag & ADS_FILTER_NOTEX)) {
        afm->items += animdata_filter_ds_texture(
            afm->ac, &afm->tmp_data, afm->ads, tex, owner_id, afm->filter_mode);
      }
      break;
    }
    case ID_NT: {
      bNodeTree *node_tree = (bNodeTree *)id;
      if (!(afm->ads->filterflag & ADS_FILTER_NONTREE)) {
        afm->items += animdata_filter_ds_nodetree(
            afm->ac, &afm->tmp_data, afm->ads, owner_id, node_tree, afm->filter_mode);
      }
    }

    /* TODO: images? */
    default:
      break;
  }
}

/* animation linked to data used by modifiers
 * NOTE: strictly speaking, modifier animation is already included under Object level
 *       but for some modifiers (e.g. Displace), there can be linked data that has settings
 *       which would be nice to animate (i.e. texture parameters) but which are not actually
 *       attached to any other objects/materials/etc. in the scene
 */
/* TODO: do we want an expander for this? */
static size_t animdata_filter_ds_modifiers(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Object *ob, int filter_mode)
{
  tAnimFilterModifiersContext afm = {nullptr};
  size_t items = 0;

  /* 1) create a temporary "context" containing all the info we have here to pass to the callback
   *    use to walk through the dependencies of the modifiers
   *
   * Assumes that all other unspecified values (i.e. accumulation buffers)
   * are zero'd out properly!
   */
  afm.ac = ac;
  afm.ads = ads;
  afm.filter_mode = filter_mode;

  /* 2) walk over dependencies */
  BKE_modifiers_foreach_ID_link(ob, animfilter_modifier_idpoin_cb, &afm);

  /* 3) extract data from the context, merging it back into the standard list */
  if (afm.items) {
    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &afm.tmp_data);
    BLI_assert(BLI_listbase_is_empty(&afm.tmp_data));
    items += afm.items;
  }

  return items;
}

/* ............ */

static size_t animdata_filter_ds_particles(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Object *ob, int filter_mode)
{
  size_t items = 0;

  LISTBASE_FOREACH (ParticleSystem *, psys, &ob->particlesystem) {
    ListBase tmp_data = {nullptr, nullptr};
    size_t tmp_items = 0;

    /* Note that when psys->part->adt is nullptr the textures can still be
     * animated. */
    if (psys->part == nullptr) {
      continue;
    }

    /* add particle-system's animation data to temp collection */
    BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_PART_OBJD(psys->part)) {
      /* particle system's animation data */
      tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)psys->part, filter_mode);

      /* textures */
      if (!(ads->filterflag & ADS_FILTER_NOTEX)) {
        tmp_items += animdata_filter_ds_textures(
            ac, &tmp_data, ads, (ID *)psys->part, filter_mode);
      }
    }
    END_ANIMFILTER_SUBCHANNELS;

    /* did we find anything? */
    if (tmp_items) {
      /* include particle-expand widget first */
      if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
        /* check if filtering by active status */
        if (ANIMCHANNEL_ACTIVEOK(psys->part)) {
          ANIMCHANNEL_NEW_CHANNEL(psys->part, ANIMTYPE_DSPART, psys->part, nullptr);
        }
      }

      /* now add the list of collected channels */
      BLI_movelisttolist(anim_data, &tmp_data);
      BLI_assert(BLI_listbase_is_empty(&tmp_data));
      items += tmp_items;
    }
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_ds_obdata(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Object *ob, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  IdAdtTemplate *iat = static_cast<IdAdtTemplate *>(ob->data);
  short type = 0, expanded = 0;

  /* get settings based on data type */
  switch (ob->type) {
    case OB_CAMERA: /* ------- Camera ------------ */
    {
      Camera *ca = (Camera *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOCAM) {
        return 0;
      }

      type = ANIMTYPE_DSCAM;
      expanded = FILTER_CAM_OBJD(ca);
      break;
    }
    case OB_LAMP: /* ---------- Light ----------- */
    {
      Light *la = (Light *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOLAM) {
        return 0;
      }

      type = ANIMTYPE_DSLAM;
      expanded = FILTER_LAM_OBJD(la);
      break;
    }
    case OB_CURVES_LEGACY: /* ------- Curve ---------- */
    case OB_SURF:          /* ------- Nurbs Surface ---------- */
    case OB_FONT:          /* ------- Text Curve ---------- */
    {
      Curve *cu = (Curve *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOCUR) {
        return 0;
      }

      type = ANIMTYPE_DSCUR;
      expanded = FILTER_CUR_OBJD(cu);
      break;
    }
    case OB_MBALL: /* ------- MetaBall ---------- */
    {
      MetaBall *mb = (MetaBall *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOMBA) {
        return 0;
      }

      type = ANIMTYPE_DSMBALL;
      expanded = FILTER_MBALL_OBJD(mb);
      break;
    }
    case OB_ARMATURE: /* ------- Armature ---------- */
    {
      bArmature *arm = (bArmature *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOARM) {
        return 0;
      }

      type = ANIMTYPE_DSARM;
      expanded = FILTER_ARM_OBJD(arm);
      break;
    }
    case OB_MESH: /* ------- Mesh ---------- */
    {
      Mesh *mesh = (Mesh *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOMESH) {
        return 0;
      }

      type = ANIMTYPE_DSMESH;
      expanded = FILTER_MESH_OBJD(mesh);
      break;
    }
    case OB_LATTICE: /* ---- Lattice ---- */
    {
      Lattice *lt = (Lattice *)ob->data;

      if (ads->filterflag & ADS_FILTER_NOLAT) {
        return 0;
      }

      type = ANIMTYPE_DSLAT;
      expanded = FILTER_LATTICE_OBJD(lt);
      break;
    }
    case OB_SPEAKER: /* ---------- Speaker ----------- */
    {
      Speaker *spk = (Speaker *)ob->data;

      type = ANIMTYPE_DSSPK;
      expanded = FILTER_SPK_OBJD(spk);
      break;
    }
    case OB_CURVES: /* ---------- Curves ----------- */
    {
      Curves *curves = (Curves *)ob->data;

      if (ads->filterflag2 & ADS_FILTER_NOHAIR) {
        return 0;
      }

      type = ANIMTYPE_DSHAIR;
      expanded = FILTER_CURVES_OBJD(curves);
      break;
    }
    case OB_POINTCLOUD: /* ---------- PointCloud ----------- */
    {
      PointCloud *pointcloud = (PointCloud *)ob->data;

      if (ads->filterflag2 & ADS_FILTER_NOPOINTCLOUD) {
        return 0;
      }

      type = ANIMTYPE_DSPOINTCLOUD;
      expanded = FILTER_POINTS_OBJD(pointcloud);
      break;
    }
    case OB_VOLUME: /* ---------- Volume ----------- */
    {
      Volume *volume = (Volume *)ob->data;

      if (ads->filterflag2 & ADS_FILTER_NOVOLUME) {
        return 0;
      }

      type = ANIMTYPE_DSVOLUME;
      expanded = FILTER_VOLUME_OBJD(volume);
      break;
    }
  }

  /* add object data animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (expanded) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)iat, filter_mode);

    /* sub-data filtering... */
    switch (ob->type) {
      case OB_LAMP: /* light - textures + nodetree */
      {
        Light *la = static_cast<Light *>(ob->data);
        bNodeTree *ntree = la->nodetree;

        /* nodetree */
        if ((ntree) && !(ads->filterflag & ADS_FILTER_NONTREE)) {
          tmp_items += animdata_filter_ds_nodetree(
              ac, &tmp_data, ads, &la->id, ntree, filter_mode);
        }
        break;
      }
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      if (ANIMCHANNEL_ACTIVEOK(iat)) {
        ANIMCHANNEL_NEW_CHANNEL(iat, type, iat, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* shapekey-level animation */
static size_t animdata_filter_ds_keyanim(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Object *ob, Key *key, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add shapekey-level animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_SKE_OBJD(key)) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)key, filter_mode);
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include key-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      if (ANIMCHANNEL_ACTIVEOK(key)) {
        ANIMCHANNEL_NEW_CHANNEL(key, ANIMTYPE_DSSKEY, ob, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* object-level animation */
static size_t animdata_filter_ds_obanim(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Object *ob, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  AnimData *adt = ob->adt;
  short type = 0, expanded = 1;
  void *cdata = nullptr;

  /* determine the type of expander channels to use */
  /* this is the best way to do this for now... */
  ANIMDATA_FILTER_CASES(
      ob, /* Some useless long comment to prevent wrapping by old clang-format versions... */
      {/* AnimData - no channel, but consider data */},
      {/* NLA - no channel, but consider data */},
      { /* Drivers */
        type = ANIMTYPE_FILLDRIVERS;
        cdata = adt;
        expanded = EXPANDED_DRVD(adt);
      },
      {/* NLA Strip Controls - no dedicated channel for now (XXX) */},
      { /* Keyframes */
        type = ANIMTYPE_FILLACTD;
        cdata = adt->action;
        expanded = EXPANDED_ACTC(adt->action);
      });

  /* add object-level animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (expanded) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)ob, filter_mode);
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include anim-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      if (type != ANIMTYPE_NONE) {
        /* NOTE: active-status (and the associated checks) don't apply here... */
        ANIMCHANNEL_NEW_CHANNEL(cdata, type, ob, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

/* get animation channels from object2 */
static size_t animdata_filter_dopesheet_ob(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Base *base, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  Object *ob = base->object;
  size_t tmp_items = 0;
  size_t items = 0;

  /* filter data contained under object first */
  BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_OBJC(ob)) {
    Key *key = BKE_key_from_object(ob);

    /* object-level animation */
    if ((ob->adt) && !(ads->filterflag & ADS_FILTER_NOOBJ)) {
      tmp_items += animdata_filter_ds_obanim(ac, &tmp_data, ads, ob, filter_mode);
    }

    /* particle deflector textures */
    if (ob->pd != nullptr && ob->pd->tex != nullptr && !(ads->filterflag & ADS_FILTER_NOTEX)) {
      tmp_items += animdata_filter_ds_texture(
          ac, &tmp_data, ads, ob->pd->tex, &ob->id, filter_mode);
    }

    /* shape-key */
    if ((key && key->adt) && !(ads->filterflag & ADS_FILTER_NOSHAPEKEYS)) {
      tmp_items += animdata_filter_ds_keyanim(ac, &tmp_data, ads, ob, key, filter_mode);
    }

    /* modifiers */
    if ((ob->modifiers.first) && !(ads->filterflag & ADS_FILTER_NOMODIFIERS)) {
      tmp_items += animdata_filter_ds_modifiers(ac, &tmp_data, ads, ob, filter_mode);
    }

    /* materials */
    if ((ob->totcol) && !(ads->filterflag & ADS_FILTER_NOMAT)) {
      tmp_items += animdata_filter_ds_materials(ac, &tmp_data, ads, ob, filter_mode);
    }

    /* object data */
    if ((ob->data) && (ob->type != OB_GPENCIL_LEGACY)) {
      tmp_items += animdata_filter_ds_obdata(ac, &tmp_data, ads, ob, filter_mode);
    }

    /* particles */
    if ((ob->particlesystem.first) && !(ads->filterflag & ADS_FILTER_NOPART)) {
      tmp_items += animdata_filter_ds_particles(ac, &tmp_data, ads, ob, filter_mode);
    }

    /* grease pencil */
    if ((ob->type == OB_GPENCIL_LEGACY) && (ob->data) && !(ads->filterflag & ADS_FILTER_NOGPENCIL))
    {
      tmp_items += animdata_filter_ds_gpencil(
          ac, &tmp_data, ads, static_cast<bGPdata *>(ob->data), filter_mode);
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* if we collected some channels, add these to the new list... */
  if (tmp_items) {
    /* firstly add object expander if required */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by selection */
      /* XXX: double-check on this -
       * most of the time, a lot of tools need to filter out these channels! */
      if (ANIMCHANNEL_SELOK((base->flag & BASE_SELECTED))) {
        /* check if filtering by active status */
        if (ANIMCHANNEL_ACTIVEOK(ob)) {
          ANIMCHANNEL_NEW_CHANNEL(base, ANIMTYPE_OBJECT, ob, nullptr);
        }
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added */
  return items;
}

static size_t animdata_filter_ds_world(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Scene *sce, World *wo, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* add world animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (FILTER_WOR_SCED(wo)) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)wo, filter_mode);

    /* nodes */
    if ((wo->nodetree) && !(ads->filterflag & ADS_FILTER_NONTREE)) {
      tmp_items += animdata_filter_ds_nodetree(
          ac, &tmp_data, ads, (ID *)wo, wo->nodetree, filter_mode);
    }
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      if (ANIMCHANNEL_ACTIVEOK(wo)) {
        ANIMCHANNEL_NEW_CHANNEL(wo, ANIMTYPE_DSWOR, sce, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_ds_scene(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Scene *sce, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  AnimData *adt = sce->adt;
  short type = 0, expanded = 1;
  void *cdata = nullptr;

  /* determine the type of expander channels to use */
  /* this is the best way to do this for now... */
  ANIMDATA_FILTER_CASES(
      sce, /* Some useless long comment to prevent wrapping by old clang-format versions... */
      {/* AnimData - no channel, but consider data */},
      {/* NLA - no channel, but consider data */},
      { /* Drivers */
        type = ANIMTYPE_FILLDRIVERS;
        cdata = adt;
        expanded = EXPANDED_DRVD(adt);
      },
      {/* NLA Strip Controls - no dedicated channel for now (XXX) */},
      { /* Keyframes */
        type = ANIMTYPE_FILLACTD;
        cdata = adt->action;
        expanded = EXPANDED_ACTC(adt->action);
      });

  /* add scene-level animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (expanded) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)sce, filter_mode);
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* did we find anything? */
  if (tmp_items) {
    /* include anim-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      if (type != ANIMTYPE_NONE) {
        /* NOTE: active-status (and the associated checks) don't apply here... */
        ANIMCHANNEL_NEW_CHANNEL(cdata, type, sce, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_dopesheet_scene(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, Scene *sce, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;

  /* filter data contained under object first */
  BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_SCEC(sce)) {
    bNodeTree *ntree = sce->nodetree;
    bGPdata *gpd = sce->gpd;
    World *wo = sce->world;

    /* Action, Drivers, or NLA for Scene */
    if ((ads->filterflag & ADS_FILTER_NOSCE) == 0) {
      tmp_items += animdata_filter_ds_scene(ac, &tmp_data, ads, sce, filter_mode);
    }

    /* world */
    if ((wo) && !(ads->filterflag & ADS_FILTER_NOWOR)) {
      tmp_items += animdata_filter_ds_world(ac, &tmp_data, ads, sce, wo, filter_mode);
    }

    /* nodetree */
    if ((ntree) && !(ads->filterflag & ADS_FILTER_NONTREE)) {
      tmp_items += animdata_filter_ds_nodetree(ac, &tmp_data, ads, (ID *)sce, ntree, filter_mode);
    }

    /* line styles */
    if ((ads->filterflag & ADS_FILTER_NOLINESTYLE) == 0) {
      tmp_items += animdata_filter_ds_linestyle(ac, &tmp_data, ads, sce, filter_mode);
    }

    /* grease pencil */
    if ((gpd) && !(ads->filterflag & ADS_FILTER_NOGPENCIL)) {
      tmp_items += animdata_filter_ds_gpencil(ac, &tmp_data, ads, gpd, filter_mode);
    }

    /* TODO: one day, when sequencer becomes its own datatype,
     * perhaps it should be included here. */
  }
  END_ANIMFILTER_SUBCHANNELS;

  /* if we collected some channels, add these to the new list... */
  if (tmp_items) {
    /* firstly add object expander if required */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by selection */
      if (ANIMCHANNEL_SELOK((sce->flag & SCE_DS_SELECTED))) {
        /* NOTE: active-status doesn't matter for this! */
        ANIMCHANNEL_NEW_CHANNEL(sce, ANIMTYPE_SCENE, sce, nullptr);
      }
    }

    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }

  /* return the number of items added */
  return items;
}

static size_t animdata_filter_ds_movieclip(
    bAnimContext *ac, ListBase *anim_data, bDopeSheet *ads, MovieClip *clip, int filter_mode)
{
  ListBase tmp_data = {nullptr, nullptr};
  size_t tmp_items = 0;
  size_t items = 0;
  /* add world animation channels */
  BEGIN_ANIMFILTER_SUBCHANNELS (EXPANDED_MCLIP(clip)) {
    /* animation data filtering */
    tmp_items += animfilter_block_data(ac, &tmp_data, ads, (ID *)clip, filter_mode);
  }
  END_ANIMFILTER_SUBCHANNELS;
  /* did we find anything? */
  if (tmp_items) {
    /* include data-expand widget first */
    if (filter_mode & ANIMFILTER_LIST_CHANNELS) {
      /* check if filtering by active status */
      if (ANIMCHANNEL_ACTIVEOK(clip)) {
        ANIMCHANNEL_NEW_CHANNEL(clip, ANIMTYPE_DSMCLIP, clip, nullptr);
      }
    }
    /* now add the list of collected channels */
    BLI_movelisttolist(anim_data, &tmp_data);
    BLI_assert(BLI_listbase_is_empty(&tmp_data));
    items += tmp_items;
  }
  /* return the number of items added to the list */
  return items;
}

static size_t animdata_filter_dopesheet_movieclips(bAnimContext *ac,
                                                   ListBase *anim_data,
                                                   bDopeSheet *ads,
                                                   int filter_mode)
{
  size_t items = 0;
  LISTBASE_FOREACH (MovieClip *, clip, &ac->bmain->movieclips) {
    /* only show if gpd is used by something... */
    if (ID_REAL_USERS(clip) < 1) {
      continue;
    }
    items += animdata_filter_ds_movieclip(ac, anim_data, ads, clip, filter_mode);
  }
  /* return the number of items added to the list */
  return items;
}

/* Helper for animdata_filter_dopesheet() - For checking if an object should be included or not */
static bool animdata_filter_base_is_ok(bDopeSheet *ads,
                                       Base *base,
                                       const eObjectMode object_mode,
                                       int filter_mode)
{
  Object *ob = base->object;

  if (base->object == nullptr) {
    return false;
  }

  /* firstly, check if object can be included, by the following factors:
   * - if only visible, must check for layer and also viewport visibility
   *   --> while tools may demand only visible, user setting takes priority
   *       as user option controls whether sets of channels get included while
   *       tool-flag takes into account collapsed/open channels too
   * - if only selected, must check if object is selected
   * - there must be animation data to edit (this is done recursively as we
   *   try to add the channels)
   */
  if ((filter_mode & ANIMFILTER_DATA_VISIBLE) && !(ads->filterflag & ADS_FILTER_INCL_HIDDEN)) {
    /* layer visibility - we check both object and base, since these may not be in sync yet */
    if ((base->flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0 ||
        (base->flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT) == 0)
    {
      return false;
    }

    /* outliner restrict-flag */
    if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
      return false;
    }
  }

  /* if only F-Curves with visible flags set can be shown, check that
   * data-block hasn't been set to invisible.
   */
  if (filter_mode & ANIMFILTER_CURVE_VISIBLE) {
    if ((ob->adt) && (ob->adt->flag & ADT_CURVES_NOT_VISIBLE)) {
      return false;
    }
  }

  /* Pinned curves are visible regardless of selection flags. */
  if ((ob->adt) && (ob->adt->flag & ADT_CURVES_ALWAYS_VISIBLE)) {
    return true;
  }

  /* Special case.
   * We don't do recursive checks for pin, but we need to deal with tricky
   * setup like animated camera lens without animated camera location.
   * Without such special handle here we wouldn't be able to bin such
   * camera data only animation to the editor.
   */
  if (ob->adt == nullptr && ob->data != nullptr) {
    AnimData *data_adt = BKE_animdata_from_id(static_cast<ID *>(ob->data));
    if (data_adt != nullptr && (data_adt->flag & ADT_CURVES_ALWAYS_VISIBLE)) {
      return true;
    }
  }

  /* check selection and object type filters */
  if (ads->filterflag & ADS_FILTER_ONLYSEL) {
    if (object_mode & OB_MODE_POSE) {
      /* When in pose-mode handle all pose-mode objects.
       * This avoids problems with pose-mode where objects may be unselected,
       * where a selected bone of an unselected object would be hidden. see: #81922. */
      if (!(base->object->mode & object_mode)) {
        return false;
      }
    }
    else {
      /* only selected should be shown (ignore active) */
      if (!(base->flag & BASE_SELECTED)) {
        return false;
      }
    }
  }

  /* check if object belongs to the filtering group if option to filter
   * objects by the grouped status is on
   * - used to ease the process of doing multiple-character choreographies
   */
  if (ads->filter_grp != nullptr) {
    if (BKE_collection_has_object_recursive(ads->filter_grp, ob) == 0) {
      return false;
    }
  }

  /* no reason to exclude this object... */
  return true;
}

/* Helper for animdata_filter_ds_sorted_bases() - Comparison callback for two Base pointers... */
static int ds_base_sorting_cmp(const void *base1_ptr, const void *base2_ptr)
{
  const Base *b1 = *((const Base **)base1_ptr);
  const Base *b2 = *((const Base **)base2_ptr);

  return strcmp(b1->object->id.name + 2, b2->object->id.name + 2);
}

/* Get a sorted list of all the bases - for inclusion in dopesheet (when drawing channels) */
static Base **animdata_filter_ds_sorted_bases(bDopeSheet *ads,
                                              const Scene *scene,
                                              ViewLayer *view_layer,
                                              int filter_mode,
                                              size_t *r_usable_bases)
{
  /* Create an array with space for all the bases, but only containing the usable ones */
  BKE_view_layer_synced_ensure(scene, view_layer);
  ListBase *object_bases = BKE_view_layer_object_bases_get(view_layer);
  size_t tot_bases = BLI_listbase_count(object_bases);
  size_t num_bases = 0;

  Base **sorted_bases = MEM_cnew_array<Base *>(tot_bases, "Dopesheet Usable Sorted Bases");
  LISTBASE_FOREACH (Base *, base, object_bases) {
    if (animdata_filter_base_is_ok(ads, base, OB_MODE_OBJECT, filter_mode)) {
      sorted_bases[num_bases++] = base;
    }
  }

  /* Sort this list of pointers (based on the names) */
  qsort(sorted_bases, num_bases, sizeof(Base *), ds_base_sorting_cmp);

  /* Return list of sorted bases */
  *r_usable_bases = num_bases;
  return sorted_bases;
}

/* TODO: implement pinning...
 * (if and when pinning is done, what we need to do is to provide freeing mechanisms -
 * to protect against data that was deleted). */
static size_t animdata_filter_dopesheet(bAnimContext *ac,
                                        ListBase *anim_data,
                                        bDopeSheet *ads,
                                        int filter_mode)
{
  Scene *scene = (Scene *)ads->source;
  ViewLayer *view_layer = (ViewLayer *)ac->view_layer;
  size_t items = 0;

  /* check that we do indeed have a scene */
  if ((ads->source == nullptr) || (GS(ads->source->name) != ID_SCE)) {
    printf("Dope Sheet Error: No scene!\n");
    if (G.debug & G_DEBUG) {
      printf("\tPointer = %p, Name = '%s'\n",
             (void *)ads->source,
             (ads->source) ? ads->source->name : nullptr);
    }
    return 0;
  }

  /* augment the filter-flags with settings based on the dopesheet filterflags
   * so that some temp settings can get added automagically...
   */
  if (ads->filterflag & ADS_FILTER_SELEDIT) {
    /* only selected F-Curves should get their keyframes considered for editability */
    filter_mode |= ANIMFILTER_SELEDIT;
  }

  /* Cache files level animations (frame duration and such). */
  if (!(ads->filterflag2 & ADS_FILTER_NOCACHEFILES) && !(ads->filterflag & ADS_FILTER_ONLYSEL)) {
    LISTBASE_FOREACH (CacheFile *, cache_file, &ac->bmain->cachefiles) {
      items += animdata_filter_ds_cachefile(ac, anim_data, ads, cache_file, filter_mode);
    }
  }

  /* movie clip's animation */
  if (!(ads->filterflag2 & ADS_FILTER_NOMOVIECLIPS) && !(ads->filterflag & ADS_FILTER_ONLYSEL)) {
    items += animdata_filter_dopesheet_movieclips(ac, anim_data, ads, filter_mode);
  }

  /* Scene-linked animation - e.g. world, compositing nodes, scene anim
   * (including sequencer currently). */
  items += animdata_filter_dopesheet_scene(ac, anim_data, ads, scene, filter_mode);

  /* If filtering for channel drawing, we want the objects in alphabetical order,
   * to make it easier to predict where items are in the hierarchy
   * - This order only really matters
   *   if we need to show all channels in the list (e.g. for drawing).
   *   (XXX: What about lingering "active" flags? The order may now become unpredictable)
   * - Don't do this if this behavior has been turned off (i.e. due to it being too slow)
   * - Don't do this if there's just a single object
   */
  BKE_view_layer_synced_ensure(scene, view_layer);
  ListBase *object_bases = BKE_view_layer_object_bases_get(view_layer);
  if ((filter_mode & ANIMFILTER_LIST_CHANNELS) && !(ads->flag & ADS_FLAG_NO_DB_SORT) &&
      (object_bases->first != object_bases->last))
  {
    /* Filter list of bases (i.e. objects), sort them, then add their contents normally... */
    /* TODO: Cache the old sorted order - if the set of bases hasn't changed, don't re-sort... */
    Base **sorted_bases;
    size_t num_bases;

    sorted_bases = animdata_filter_ds_sorted_bases(
        ads, scene, view_layer, filter_mode, &num_bases);
    if (sorted_bases) {
      /* Add the necessary channels for these bases... */
      for (size_t i = 0; i < num_bases; i++) {
        items += animdata_filter_dopesheet_ob(ac, anim_data, ads, sorted_bases[i], filter_mode);
      }

      /* TODO: store something to validate whether any changes are needed? */

      /* free temporary data */
      MEM_freeN(sorted_bases);
    }
  }
  else {
    /* Filter and add contents of each base (i.e. object) without them sorting first
     * NOTE: This saves performance in cases where order doesn't matter
     */
    Object *obact = BKE_view_layer_active_object_get(view_layer);
    const eObjectMode object_mode = (obact != nullptr) ? eObjectMode(obact->mode) : OB_MODE_OBJECT;
    LISTBASE_FOREACH (Base *, base, object_bases) {
      if (animdata_filter_base_is_ok(ads, base, object_mode, filter_mode)) {
        /* since we're still here, this object should be usable */
        items += animdata_filter_dopesheet_ob(ac, anim_data, ads, base, filter_mode);
      }
    }
  }

  /* return the number of items in the list */
  return items;
}

/* Summary track for DopeSheet/Action Editor
 * - return code is whether the summary lets the other channels get drawn
 */
static short animdata_filter_dopesheet_summary(bAnimContext *ac,
                                               ListBase *anim_data,
                                               int filter_mode,
                                               size_t *items)
{
  bDopeSheet *ads = nullptr;

  /* get the DopeSheet information to use
   * - we should only need to deal with the DopeSheet/Action Editor,
   *   since all the other Animation Editors won't have this concept
   *   being applicable.
   */
  if ((ac && ac->sl) && (ac->spacetype == SPACE_ACTION)) {
    SpaceAction *saction = (SpaceAction *)ac->sl;
    ads = &saction->ads;
  }
  else {
    /* invalid space type - skip this summary channels */
    return 1;
  }

  /* dopesheet summary
   * - only for drawing and/or selecting keyframes in channels, but not for real editing
   * - only useful for DopeSheet/Action/etc. editors where it is actually useful
   */
  if ((filter_mode & ANIMFILTER_LIST_CHANNELS) && (ads->filterflag & ADS_FILTER_SUMMARY)) {
    bAnimListElem *ale = make_new_animlistelem(ac, ANIMTYPE_SUMMARY, nullptr, nullptr);
    if (ale) {
      BLI_addtail(anim_data, ale);
      (*items)++;
    }

    /* If summary is collapsed, don't show other channels beneath this - this check is put inside
     * the summary check so that it doesn't interfere with normal operation.
     */
    if (ads->flag & ADS_FLAG_SUMMARY_COLLAPSED) {
      return 0;
    }
  }

  /* the other channels beneath this can be shown */
  return 1;
}

/* ......................... */

/* filter data associated with a channel - usually for handling summary-channels in DopeSheet */
static size_t animdata_filter_animchan(bAnimContext *ac,
                                       ListBase *anim_data,
                                       bDopeSheet *ads,
                                       bAnimListElem *channel,
                                       int filter_mode)
{
  size_t items = 0;

  /* data to filter depends on channel type */
  /* NOTE: only common channel-types have been handled for now. More can be added as necessary */
  switch (channel->type) {
    case ANIMTYPE_SUMMARY:
      items += animdata_filter_dopesheet(ac, anim_data, ads, filter_mode);
      break;

    case ANIMTYPE_SCENE:
      items += animdata_filter_dopesheet_scene(
          ac, anim_data, ads, static_cast<Scene *>(channel->data), filter_mode);
      break;

    case ANIMTYPE_OBJECT:
      items += animdata_filter_dopesheet_ob(
          ac, anim_data, ads, static_cast<Base *>(channel->data), filter_mode);
      break;

    case ANIMTYPE_DSCACHEFILE:
      items += animdata_filter_ds_cachefile(
          ac, anim_data, ads, static_cast<CacheFile *>(channel->data), filter_mode);
      break;

    case ANIMTYPE_ANIMDATA:
      items += animfilter_block_data(ac, anim_data, ads, channel->id, filter_mode);
      break;

    default:
      printf("ERROR: Unsupported channel type (%d) in animdata_filter_animchan()\n",
             channel->type);
      break;
  }

  return items;
}

/* ----------- Cleanup API --------------- */

/* Remove entries with invalid types in animation channel list */
static size_t animdata_filter_remove_invalid(ListBase *anim_data)
{
  size_t items = 0;

  /* only keep entries with valid types */
  LISTBASE_FOREACH_MUTABLE (bAnimListElem *, ale, anim_data) {
    if (ale->type == ANIMTYPE_NONE) {
      BLI_freelinkN(anim_data, ale);
    }
    else {
      items++;
    }
  }

  return items;
}

/* Remove duplicate entries in animation channel list */
static size_t animdata_filter_remove_duplis(ListBase *anim_data)
{
  GSet *gs;
  size_t items = 0;

  /* Build new hash-table to efficiently store and retrieve which entries have been
   * encountered already while searching. */
  gs = BLI_gset_ptr_new(__func__);

  /* loop through items, removing them from the list if a similar item occurs already */
  LISTBASE_FOREACH_MUTABLE (bAnimListElem *, ale, anim_data) {
    /* check if hash has any record of an entry like this
     * - just use ale->data for now, though it would be nicer to involve
     *   ale->type in combination too to capture corner cases
     *   (where same data performs differently)
     */
    if (BLI_gset_add(gs, ale->data)) {
      /* this entry is 'unique' and can be kept */
      items++;
    }
    else {
      /* this entry isn't needed anymore */
      BLI_freelinkN(anim_data, ale);
    }
  }

  /* free the hash... */
  BLI_gset_free(gs, nullptr);

  /* return the number of items still in the list */
  return items;
}

/* ----------- Public API --------------- */

size_t ANIM_animdata_filter(bAnimContext *ac,
                            ListBase *anim_data,
                            eAnimFilter_Flags filter_mode,
                            void *data,
                            eAnimCont_Types datatype)
{
  size_t items = 0;

  /* only filter data if there's somewhere to put it */
  if (data && anim_data) {
    /* firstly filter the data */
    switch (datatype) {
      /* Action-Editing Modes */
      case ANIMCONT_ACTION: /* 'Action Editor' */
      {
        Object *obact = ac->obact;
        SpaceAction *saction = (SpaceAction *)ac->sl;
        bDopeSheet *ads = (saction) ? &saction->ads : nullptr;

        /* specially check for AnimData filter, see #36687. */
        if (UNLIKELY(filter_mode & ANIMFILTER_ANIMDATA)) {
          /* all channels here are within the same AnimData block, hence this special case */
          if (LIKELY(obact->adt)) {
            ANIMCHANNEL_NEW_CHANNEL(obact->adt, ANIMTYPE_ANIMDATA, (ID *)obact, nullptr);
          }
        }
        else {
          /* The check for the DopeSheet summary is included here
           * since the summary works here too. */
          if (animdata_filter_dopesheet_summary(ac, anim_data, filter_mode, &items)) {
            items += animfilter_action(
                ac, anim_data, ads, static_cast<bAction *>(data), filter_mode, (ID *)obact);
          }
        }

        break;
      }
      case ANIMCONT_SHAPEKEY: /* 'ShapeKey Editor' */
      {
        Key *key = (Key *)data;

        /* specially check for AnimData filter, see #36687. */
        if (UNLIKELY(filter_mode & ANIMFILTER_ANIMDATA)) {
          /* all channels here are within the same AnimData block, hence this special case */
          if (LIKELY(key->adt)) {
            ANIMCHANNEL_NEW_CHANNEL(key->adt, ANIMTYPE_ANIMDATA, (ID *)key, nullptr);
          }
        }
        else {
          /* The check for the DopeSheet summary is included here
           * since the summary works here too. */
          if (animdata_filter_dopesheet_summary(ac, anim_data, filter_mode, &items)) {
            items = animdata_filter_shapekey(ac, anim_data, key, filter_mode);
          }
        }

        break;
      }

      /* Modes for Specialty Data Types (i.e. not keyframes) */
      case ANIMCONT_GPENCIL: {
        if (animdata_filter_dopesheet_summary(ac, anim_data, filter_mode, &items)) {
          if (U.experimental.use_grease_pencil_version3) {
            items = animdata_filter_grease_pencil(ac, anim_data, filter_mode);
          }
          else {
            items = animdata_filter_gpencil_legacy(ac, anim_data, data, filter_mode);
          }
        }
        break;
      }
      case ANIMCONT_MASK: {
        if (animdata_filter_dopesheet_summary(ac, anim_data, filter_mode, &items)) {
          items = animdata_filter_mask(ac->bmain, anim_data, data, filter_mode);
        }
        break;
      }

      /* DopeSheet Based Modes */
      case ANIMCONT_DOPESHEET: /* 'DopeSheet Editor' */
      {
        /* the DopeSheet editor is the primary place where the DopeSheet summaries are useful */
        if (animdata_filter_dopesheet_summary(ac, anim_data, filter_mode, &items)) {
          items += animdata_filter_dopesheet(
              ac, anim_data, static_cast<bDopeSheet *>(data), filter_mode);
        }
        break;
      }
      case ANIMCONT_FCURVES: /* Graph Editor -> F-Curves/Animation Editing */
      case ANIMCONT_DRIVERS: /* Graph Editor -> Drivers Editing */
      case ANIMCONT_NLA:     /* NLA Editor */
      {
        /* all of these editors use the basic DopeSheet data for filtering options,
         * but don't have all the same features */
        items = animdata_filter_dopesheet(
            ac, anim_data, static_cast<bDopeSheet *>(data), filter_mode);
        break;
      }

      /* Timeline Mode - Basically the same as dopesheet,
       * except we only have the summary for now */
      case ANIMCONT_TIMELINE: {
        /* the DopeSheet editor is the primary place where the DopeSheet summaries are useful */
        if (animdata_filter_dopesheet_summary(ac, anim_data, filter_mode, &items)) {
          items += animdata_filter_dopesheet(
              ac, anim_data, static_cast<bDopeSheet *>(data), filter_mode);
        }
        break;
      }

      /* Special/Internal Use */
      case ANIMCONT_CHANNEL: /* animation channel */
      {
        bDopeSheet *ads = ac->ads;

        /* based on the channel type, filter relevant data for this */
        items = animdata_filter_animchan(
            ac, anim_data, ads, static_cast<bAnimListElem *>(data), filter_mode);
        break;
      }

      /* unhandled */
      default: {
        printf("ANIM_animdata_filter() - Invalid datatype argument %i\n", datatype);
        break;
      }
    }

    /* remove any 'weedy' entries */
    items = animdata_filter_remove_invalid(anim_data);

    /* remove duplicates (if required) */
    if (filter_mode & ANIMFILTER_NODUPLIS) {
      items = animdata_filter_remove_duplis(anim_data);
    }
  }

  /* return the number of items in the list */
  return items;
}

/* ************************************************************ */
