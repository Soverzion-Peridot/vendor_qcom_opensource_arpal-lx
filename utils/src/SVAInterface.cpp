/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "PAL: SVAInterface"

#include "SVAInterface.h"

#include "detection_cmn_api.h"

#define ST_MAX_FSTAGE_CONF_LEVEL  (100)
#define CUSTOM_CONFIG_OPAQUE_DATA_SIZE 12
#define CONF_LEVELS_INTF_VERSION_0002 0x02

SVAInterface::SVAInterface(std::shared_ptr<VUIStreamConfig> sm_cfg) {
    sm_cfg_ = sm_cfg;
    hist_duration_ = 0;
    preroll_duration_ = 0;
    conf_levels_intf_version_ = 0;
    st_conf_levels_ = nullptr;
    st_conf_levels_v2_ = nullptr;
    det_model_id_ = 0;
    sm_merged_ = false;
    sound_model_info_ = new SoundModelInfo();
    std::memset(&detection_event_info_, 0,
                sizeof(struct detection_event_info));
    std::memset(&detection_event_info_multi_model_, 0,
                sizeof(struct detection_event_info_pdk));
}

SVAInterface::~SVAInterface() {
    if (st_conf_levels_) {
        free(st_conf_levels_);
        st_conf_levels_ = nullptr;
    }
    if (st_conf_levels_v2_) {
        free(st_conf_levels_v2_);
        st_conf_levels_v2_ = nullptr;
    }
    if (sound_model_info_) {
        delete sound_model_info_;
    }
}

int32_t SVAInterface::ParseSoundModel(std::shared_ptr<VUIStreamConfig> sm_cfg,
                                      struct pal_st_sound_model *sound_model,
                                      st_module_type_t &first_stage_type,
                                      std::vector<sm_pair_t> &model_list) {

    int32_t status = 0;
    int32_t i = 0;
    struct pal_st_phrase_sound_model *phrase_sm = nullptr;
    struct pal_st_sound_model *common_sm = nullptr;
    uint8_t *ptr = nullptr;
    uint8_t *sm_payload = nullptr;
    uint8_t *sm_data = nullptr;
    int32_t sm_size = 0;
    SML_GlobalHeaderType *global_hdr = nullptr;
    SML_HeaderTypeV3 *hdr_v3 = nullptr;
    SML_BigSoundModelTypeV3 *big_sm = nullptr;
    uint32_t offset = 0;

    PAL_DBG(LOG_TAG, "Enter");

    if (sound_model->type == PAL_SOUND_MODEL_TYPE_KEYPHRASE) {
        phrase_sm = (struct pal_st_phrase_sound_model *)sound_model;
        sm_payload = (uint8_t *)phrase_sm + phrase_sm->common.data_offset;
        global_hdr = (SML_GlobalHeaderType *)sm_payload;
        if (global_hdr->magicNumber == SML_GLOBAL_HEADER_MAGIC_NUMBER) {
            hdr_v3 = (SML_HeaderTypeV3 *)(sm_payload +
                                          sizeof(SML_GlobalHeaderType));
            PAL_INFO(LOG_TAG, "num of sound models = %u", hdr_v3->numModels);
            for (i = 0; i < hdr_v3->numModels; i++) {
                big_sm = (SML_BigSoundModelTypeV3 *)(
                    sm_payload + sizeof(SML_GlobalHeaderType) +
                    sizeof(SML_HeaderTypeV3) +
                    (i * sizeof(SML_BigSoundModelTypeV3)));

                PAL_INFO(LOG_TAG, "type = %u, size = %u, version = %u.%u",
                         big_sm->type, big_sm->size,
                         big_sm->versionMajor, big_sm->versionMinor);
                if (big_sm->type == ST_SM_ID_SVA_F_STAGE_GMM) {
                    first_stage_type = (st_module_type_t)big_sm->versionMajor;
                    sm_size = big_sm->size;
                    sm_data = (uint8_t *)calloc(1, sm_size);
                    if (!sm_data) {
                        status = -ENOMEM;
                        PAL_ERR(LOG_TAG, "sm_data allocation failed, status %d",
                                status);
                        goto error_exit;
                    }
                    ptr = (uint8_t *)sm_payload +
                        sizeof(SML_GlobalHeaderType) +
                        sizeof(SML_HeaderTypeV3) +
                        (hdr_v3->numModels * sizeof(SML_BigSoundModelTypeV3)) +
                        big_sm->offset;
                    ar_mem_cpy(sm_data, sm_size, ptr, sm_size);
                    model_list.push_back(std::make_pair(big_sm->type,
                                            std::make_pair(sm_data, sm_size)));
                } else if (big_sm->type != SML_ID_SVA_S_STAGE_UBM) {
                    if (big_sm->type == SML_ID_SVA_F_STAGE_INTERNAL ||
                        (big_sm->type == ST_SM_ID_SVA_S_STAGE_USER &&
                         !(phrase_sm->phrases[0].recognition_mode &
                         PAL_RECOGNITION_MODE_USER_IDENTIFICATION)))
                        continue;
                    sm_size = big_sm->size;
                    ptr = (uint8_t *)sm_payload +
                        sizeof(SML_GlobalHeaderType) +
                        sizeof(SML_HeaderTypeV3) +
                        (hdr_v3->numModels * sizeof(SML_BigSoundModelTypeV3)) +
                        big_sm->offset;
                    sm_data = (uint8_t *)calloc(1, sm_size);
                    if (!sm_data) {
                        status = -ENOMEM;
                        PAL_ERR(LOG_TAG, "Failed to alloc memory for sm_data");
                        goto error_exit;
                    }
                    ar_mem_cpy(sm_data, sm_size, ptr, sm_size);
                    model_list.push_back(std::make_pair(big_sm->type,
                                            std::make_pair(sm_data, sm_size)));
                }
            }
        } else {
            // Parse sound model 2.0
            first_stage_type = sm_cfg->GetVUIModuleType();
            sm_size = phrase_sm->common.data_size;
            sm_data = (uint8_t *)calloc(1, sm_size);
            if (!sm_data) {
                PAL_ERR(LOG_TAG, "Failed to allocate memory for sm_data");
                status = -ENOMEM;
                goto error_exit;
            }
            ptr = (uint8_t*)phrase_sm + phrase_sm->common.data_offset;
            ar_mem_cpy(sm_data, sm_size, ptr, sm_size);
            model_list.push_back(std::make_pair(ST_SM_ID_SVA_F_STAGE_GMM,
                                                std::make_pair(sm_data, sm_size)));
        }
    } else {
        // handle for generic sound model
        first_stage_type = sm_cfg->GetVUIModuleType();
        common_sm = sound_model;
        sm_size = common_sm->data_size;
        sm_data = (uint8_t *)calloc(1, sm_size);
        if (!sm_data) {
            PAL_ERR(LOG_TAG, "Failed to allocate memory for sm_data");
            status = -ENOMEM;
            goto error_exit;
        }
        ptr = (uint8_t*)common_sm + common_sm->data_offset;
        ar_mem_cpy(sm_data, sm_size, ptr, sm_size);
        model_list.push_back(std::make_pair(ST_SM_ID_SVA_F_STAGE_GMM,
                                            std::make_pair(sm_data, sm_size)));
    }
    PAL_DBG(LOG_TAG, "Exit, status %d", status);
    return status;

error_exit:
    // clean up memory added to model_list in failure case
    for (auto iter = model_list.begin(); iter != model_list.end(); iter++) {
        if ((*iter).second.first)
            free((*iter).second.first);
    }
    model_list.clear();

    PAL_DBG(LOG_TAG, "Exit, status %d", status);
    return status;
}

int32_t SVAInterface::ParseRecognitionConfig(Stream *s,
    struct pal_st_recognition_config *config) {

    int32_t status = 0;
    struct sound_model_info *sm_info = nullptr;
    struct st_param_header *param_hdr = NULL;
    struct st_hist_buffer_info *hist_buf = NULL;
    struct st_det_perf_mode_info *det_perf_mode = NULL;
    uint8_t *opaque_ptr = NULL;
    unsigned int opaque_size = 0, conf_levels_payload_size = 0;
    uint32_t hist_buffer_duration = 0;
    uint32_t pre_roll_duration = 0;
    uint8_t *conf_levels = NULL;
    uint32_t num_conf_levels = 0;

    if (sm_info_map_.find(s) != sm_info_map_.end() && sm_info_map_[s]) {
        sm_info = sm_info_map_[s];
        sm_info->rec_config = config;
    } else {
        PAL_ERR(LOG_TAG, "Stream not registered to interface");
        return -EINVAL;
    }

    PAL_DBG(LOG_TAG, "Enter");
    if (!config) {
        PAL_ERR(LOG_TAG, "Invalid config");
        return -EINVAL;
    }

    // Parse recognition config
    if (config->data_size > CUSTOM_CONFIG_OPAQUE_DATA_SIZE &&
        sm_cfg_->isQCVAUUID()) {
        opaque_ptr = (uint8_t *)config + config->data_offset;
        while (opaque_size < config->data_size) {
            param_hdr = (struct st_param_header *)opaque_ptr;
            PAL_VERBOSE(LOG_TAG, "key %d, payload size %d",
                        param_hdr->key_id, param_hdr->payload_size);

            switch (param_hdr->key_id) {
              case ST_PARAM_KEY_CONFIDENCE_LEVELS:
                  conf_levels_intf_version_ = *(uint32_t *)(
                      opaque_ptr + sizeof(struct st_param_header));
                  PAL_VERBOSE(LOG_TAG, "conf_levels_intf_version = %u",
                      conf_levels_intf_version_);
                  if (conf_levels_intf_version_ !=
                      CONF_LEVELS_INTF_VERSION_0002) {
                      conf_levels_payload_size =
                          sizeof(struct st_confidence_levels_info);
                  } else {
                      conf_levels_payload_size =
                          sizeof(struct st_confidence_levels_info_v2);
                  }
                  if (param_hdr->payload_size != conf_levels_payload_size) {
                      PAL_ERR(LOG_TAG, "Conf level format error, exiting");
                      status = -EINVAL;
                      goto error_exit;
                  }
                  status = ParseOpaqueConfLevels(sm_info, opaque_ptr,
                                                 conf_levels_intf_version_,
                                                 &conf_levels,
                                                 &num_conf_levels);
                if (status) {
                    PAL_ERR(LOG_TAG, "Failed to parse opaque conf levels");
                    goto error_exit;
                }

                opaque_size += sizeof(struct st_param_header) +
                    conf_levels_payload_size;
                opaque_ptr += sizeof(struct st_param_header) +
                    conf_levels_payload_size;
                if (status) {
                    PAL_ERR(LOG_TAG, "Parse conf levels failed(status=%d)",
                            status);
                    status = -EINVAL;
                    goto error_exit;
                }
                break;
              case ST_PARAM_KEY_HISTORY_BUFFER_CONFIG:
                  if (param_hdr->payload_size !=
                      sizeof(struct st_hist_buffer_info)) {
                      PAL_ERR(LOG_TAG, "History buffer config format error");
                      status = -EINVAL;
                      goto error_exit;
                  }
                  hist_buf = (struct st_hist_buffer_info *)(opaque_ptr +
                      sizeof(struct st_param_header));
                  hist_buffer_duration = hist_buf->hist_buffer_duration_msec;
                  pre_roll_duration = hist_buf->pre_roll_duration_msec;

                  opaque_size += sizeof(struct st_param_header) +
                      sizeof(struct st_hist_buffer_info);
                  opaque_ptr += sizeof(struct st_param_header) +
                      sizeof(struct st_hist_buffer_info);
                  break;
              case ST_PARAM_KEY_DETECTION_PERF_MODE:
                  if (param_hdr->payload_size !=
                      sizeof(struct st_det_perf_mode_info)) {
                      PAL_ERR(LOG_TAG, "Opaque data format error, exiting");
                      status = -EINVAL;
                      goto error_exit;
                  }
                  det_perf_mode = (struct st_det_perf_mode_info *)
                      (opaque_ptr + sizeof(struct st_param_header));
                  PAL_DBG(LOG_TAG, "set perf mode %d", det_perf_mode->mode);
                  opaque_size += sizeof(struct st_param_header) +
                      sizeof(struct st_det_perf_mode_info);
                  opaque_ptr += sizeof(struct st_param_header) +
                      sizeof(struct st_det_perf_mode_info);
                  break;
              default:
                  PAL_ERR(LOG_TAG, "Unsupported opaque data key id, exiting");
                  status = -EINVAL;
                  goto error_exit;
            }
        }
    } else {
        // get history buffer duration from sound trigger platform xml
        hist_buffer_duration = sm_cfg_->GetKwDuration();
        pre_roll_duration = 0;

        if (sm_cfg_->isQCVAUUID()) {
            status = FillConfLevels(sm_info, config, &conf_levels, &num_conf_levels);
            if (status) {
                PAL_ERR(LOG_TAG, "Failed to parse conf levels from rc config");
                goto error_exit;
            }
        }
    }

    sm_info_map_[s]->hist_buffer_duration = hist_buffer_duration;
    sm_info_map_[s]->pre_roll_duration = pre_roll_duration;

    if (sm_info_map_[s]->wakeup_config)
        free(sm_info_map_[s]->wakeup_config);
    sm_info_map_[s]->wakeup_config = conf_levels;
    sm_info_map_[s]->wakeup_config_size = num_conf_levels;
    goto exit;

error_exit:
    if (st_conf_levels_) {
        free(st_conf_levels_);
        st_conf_levels_ = nullptr;
    }
    if (st_conf_levels_v2_) {
        free(st_conf_levels_v2_);
        st_conf_levels_v2_ = nullptr;
    }

exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);
    return status;
}

void SVAInterface::GetWakeupConfigs(Stream *s,
                                    void **config,
                                    uint32_t *size) {

    if (sm_info_map_.find(s) != sm_info_map_.end() && sm_info_map_[s]) {
        *config = sm_info_map_[s]->wakeup_config;
        *size = sm_info_map_[s]->wakeup_config_size;
    } else {
        PAL_ERR(LOG_TAG, "Stream not registered to interface");
    }
}

void SVAInterface::GetBufferingConfigs(Stream *s,
                                       uint32_t *hist_duration,
                                       uint32_t *preroll_duration) {

    if (sm_info_map_.find(s) != sm_info_map_.end() && sm_info_map_[s]) {
        *hist_duration = sm_info_map_[s]->hist_buffer_duration;
        *preroll_duration = sm_info_map_[s]->pre_roll_duration;
    } else {
        PAL_ERR(LOG_TAG, "Stream not registered to interface");
    }
}

void SVAInterface::GetSecondStageConfLevels(Stream *s,
                                            listen_model_indicator_enum type,
                                            uint32_t *level) {

    if (sm_info_map_.find(s) != sm_info_map_.end() && sm_info_map_[s]) {
        for (auto iter = sm_info_map_[s]->sec_threshold.begin();
            iter != sm_info_map_[s]->sec_threshold.end(); iter++) {
            if ((*iter).first == type)
                *level = (*iter).second;
        }
    } else {
        PAL_ERR(LOG_TAG, "Stream not registered to interface");
    }
}

void SVAInterface::SetSecondStageDetLevels(Stream *s,
                                           listen_model_indicator_enum type,
                                           uint32_t level) {

    bool sec_det_level_exist = false;

    if (sm_info_map_.find(s) != sm_info_map_.end() && sm_info_map_[s]) {
        for (auto &iter: sm_info_map_[s]->sec_det_level) {
            if (iter.first == type) {
                iter.second = level;
                sec_det_level_exist = true;
                break;
            }
        }
        if (!sec_det_level_exist)
            sm_info_map_[s]->sec_det_level.push_back(std::make_pair(type, level));
    } else {
        PAL_ERR(LOG_TAG, "Stream not registered to interface");
    }
}

int32_t SVAInterface::ParseDetectionPayload(void *event, uint32_t size) {
    int32_t status = 0;

    if (!IS_MODULE_TYPE_PDK(module_type_)) {
        status = ParseDetectionPayloadGMM(event);
        CheckAndSetDetectionConfLevels(GetDetectedStream());
    } else {
        status = ParseDetectionPayloadPDK(event);
    }
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to parse detection payload, status %d",
                status);
    }

    return status;
}

Stream* SVAInterface::GetDetectedStream() {
    Stream *st = nullptr;
    struct sound_model_info *sm_info = nullptr;

    PAL_DBG(LOG_TAG, "Enter");
    if (sm_info_map_.empty()) {
        PAL_ERR(LOG_TAG, "Unexpected, No streams attached to engine!");
        return nullptr;
    }
    /*
     * If only single stream exists, this detection is not for merged/multi
     * sound model, hence return this as only available stream
     */
    if (!IS_MODULE_TYPE_PDK(module_type_)) {
        if (sm_info_map_.size() == 1) {
            return sm_info_map_.begin()->first;
        }

        if (detection_event_info_.num_confidence_levels <
                sound_model_info_->GetNumKeyPhrases()) {
            PAL_ERR(LOG_TAG, "detection event conf levels %d < num of keyphrases %d",
                detection_event_info_.num_confidence_levels,
                sound_model_info_->GetNumKeyPhrases());
            return nullptr;
        }

        /*
         * The DSP payload contains the keyword conf levels from the beginning.
         * Only one keyword conf level is expected to be non-zero from keyword
         * detection. Find non-zero conf level up to number of keyphrases and
         * if one is found, match it to the corresponding keyphrase from list
         * of streams to obtain the detected stream.
         */
        for (uint32_t i = 0; i < sound_model_info_->GetNumKeyPhrases(); i++) {
            if (!detection_event_info_.confidence_levels[i])
                continue;
            for (auto &iter: sm_info_map_) {
                for (uint32_t k = 0; k < iter.second->info->GetNumKeyPhrases(); k++) {
                    if (!strcmp(sound_model_info_->GetKeyPhrases()[i],
                                iter.second->info->GetKeyPhrases()[k])) {
                        return iter.first;
                    }
                }
            }
        }
    } else {
        for (auto &iter : sm_info_map_) {
            sm_info = iter.second;
            if (sm_info->model_id == det_model_id_) {
                st = iter.first;
                break;
            }
        }
        if (!st) {
            PAL_ERR(LOG_TAG, "Invalid model id = %x", det_model_id_);
        }
        return st;
    }
    return nullptr;
}

void* SVAInterface::GetDetectionEventInfo() {
    if (IS_MODULE_TYPE_PDK(module_type_)) {
       return &detection_event_info_multi_model_;
    }
    return &detection_event_info_;
}

int32_t SVAInterface::GenerateCallbackEvent(Stream *s,
                                            struct pal_st_recognition_event **event,
                                            uint32_t *size,
                                            bool detection) {

    struct sound_model_info *sm_info = nullptr;
    struct pal_st_phrase_recognition_event *phrase_event = nullptr;
    struct pal_st_generic_recognition_event *generic_event = nullptr;
    struct st_param_header *param_hdr = nullptr;
    struct st_keyword_indices_info *kw_indices = nullptr;
    struct st_timestamp_info *timestamps = nullptr;
    struct model_stats *det_model_stat = nullptr;
    struct detection_event_info_pdk *det_ev_info_pdk = nullptr;
    struct detection_event_info *det_ev_info = nullptr;
    struct pal_stream_attributes strAttr;
    size_t opaque_size = 0;
    size_t event_size = 0, conf_levels_size = 0;
    uint8_t *opaque_data = nullptr;
    uint8_t *custom_event = nullptr;
    uint32_t det_keyword_id = 0;
    uint32_t best_conf_level = 0;
    uint32_t detection_timestamp_lsw = 0;
    uint32_t detection_timestamp_msw = 0;
    int32_t status = 0;
    int32_t num_models = 0;

    if (sm_info_map_.find(s) != sm_info_map_.end() && sm_info_map_[s]) {
        sm_info = sm_info_map_[s];
    } else {
        PAL_ERR(LOG_TAG, "Stream not registered to interface");
        return -EINVAL;
    }

    status = s->getStreamAttributes(&strAttr);

    PAL_DBG(LOG_TAG, "Enter");
    *event = nullptr;
    if (sm_info->type == PAL_SOUND_MODEL_TYPE_KEYPHRASE) {
        if (sm_info->model_id > 0) {
            det_ev_info_pdk = &detection_event_info_multi_model_;
            if (!det_ev_info_pdk) {
                PAL_ERR(LOG_TAG, "detection info multi model not available");
                status = -EINVAL;
                goto exit;
            }
        } else {
            det_ev_info = &detection_event_info_;
            if (!det_ev_info) {
                PAL_ERR(LOG_TAG, "detection info not available");
                status = -EINVAL;
                goto exit;
            }
        }

        if (conf_levels_intf_version_ != CONF_LEVELS_INTF_VERSION_0002)
            conf_levels_size = sizeof(struct st_confidence_levels_info);
        else
            conf_levels_size = sizeof(struct st_confidence_levels_info_v2);

        opaque_size = (3 * sizeof(struct st_param_header)) +
            sizeof(struct st_timestamp_info) +
            sizeof(struct st_keyword_indices_info) +
            conf_levels_size;

        event_size = sizeof(struct pal_st_phrase_recognition_event) +
                     opaque_size;
        phrase_event = (struct pal_st_phrase_recognition_event *)
                       calloc(1, event_size);
        if (!phrase_event) {
            PAL_ERR(LOG_TAG, "Failed to alloc memory for recognition event");
            status =  -ENOMEM;
            goto exit;
        }

        phrase_event->num_phrases = sm_info->rec_config->num_phrases;
        memcpy(phrase_event->phrase_extras, sm_info->rec_config->phrases,
               phrase_event->num_phrases *
               sizeof(struct pal_st_phrase_recognition_extra));

        *event = &(phrase_event->common);
        (*event)->status = detection ? PAL_RECOGNITION_STATUS_SUCCESS :
                           PAL_RECOGNITION_STATUS_FAILURE;
        (*event)->type = sm_info->type;
        (*event)->st_handle = (pal_st_handle_t *)this;
        (*event)->capture_available = sm_info->rec_config->capture_requested;
        // TODO: generate capture session
        (*event)->capture_session = 0;
        (*event)->capture_delay_ms = 0;
        (*event)->capture_preamble_ms = 0;
        (*event)->trigger_in_data = true;
        (*event)->data_size = opaque_size;
        (*event)->data_offset = sizeof(struct pal_st_phrase_recognition_event);
        (*event)->media_config.sample_rate =
            strAttr.in_media_config.sample_rate;
        (*event)->media_config.bit_width =
            strAttr.in_media_config.bit_width;
        (*event)->media_config.ch_info.channels =
            strAttr.in_media_config.ch_info.channels;
        (*event)->media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
        // Filling Opaque data
        opaque_data = (uint8_t *)phrase_event +
                       phrase_event->common.data_offset;

        /* Pack the opaque data confidence levels structure */
        param_hdr = (struct st_param_header *)opaque_data;
        param_hdr->key_id = ST_PARAM_KEY_CONFIDENCE_LEVELS;
        if (conf_levels_intf_version_ !=  CONF_LEVELS_INTF_VERSION_0002)
            param_hdr->payload_size = sizeof(struct st_confidence_levels_info);
        else
            param_hdr->payload_size = sizeof(struct st_confidence_levels_info_v2);
        opaque_data += sizeof(struct st_param_header);
        /* Copy the cached conf levels from recognition config */
        if (conf_levels_intf_version_ != CONF_LEVELS_INTF_VERSION_0002)
            ar_mem_cpy(opaque_data, param_hdr->payload_size,
                    st_conf_levels_, param_hdr->payload_size);
        else
            ar_mem_cpy(opaque_data, param_hdr->payload_size,
                st_conf_levels_v2_, param_hdr->payload_size);
        if (sm_info->model_id > 0) {
            num_models = det_ev_info_pdk->num_detected_models;
            for (int i = 0; i < num_models; ++i) {
                det_model_stat = &det_ev_info_pdk->detected_model_stats[i];
                if (sm_info->model_id == det_model_stat->detected_model_id) {
                    det_keyword_id = det_model_stat->detected_keyword_id;
                    best_conf_level = det_model_stat->best_confidence_level;
                    detection_timestamp_lsw =
                        det_model_stat->detection_timestamp_lsw;
                    detection_timestamp_msw =
                        det_model_stat->detection_timestamp_msw;
                    PAL_INFO(LOG_TAG, "keywordID: %u, best_conf_level: %u",
                            det_keyword_id, best_conf_level);
                    break;
                }
            }
            FillCallbackConfLevels(sm_info, opaque_data, det_keyword_id, best_conf_level);
        } else {
            PackEventConfLevels(sm_info, opaque_data);
        }
        opaque_data += param_hdr->payload_size;

        /* Pack the opaque data keyword indices structure */
        param_hdr = (struct st_param_header *)opaque_data;
        param_hdr->key_id = ST_PARAM_KEY_KEYWORD_INDICES;
        param_hdr->payload_size = sizeof(struct st_keyword_indices_info);
        opaque_data += sizeof(struct st_param_header);
        kw_indices = (struct st_keyword_indices_info *)opaque_data;
        kw_indices->version = 0x1;

        kw_indices->start_index = start_index_;
        kw_indices->end_index = end_index_;
        opaque_data += sizeof(struct st_keyword_indices_info);

        /*
         * Pack the opaque data detection time structure
         * TODO: add support for 2nd stage detection timestamp
         */
        param_hdr = (struct st_param_header *)opaque_data;
        param_hdr->key_id = ST_PARAM_KEY_TIMESTAMP;
        param_hdr->payload_size = sizeof(struct st_timestamp_info);
        opaque_data += sizeof(struct st_param_header);
        timestamps = (struct st_timestamp_info *)opaque_data;
        timestamps->version = 0x1;
        if (sm_info->model_id > 0) {
            timestamps->first_stage_det_event_time = 1000 *
                        ((uint64_t)detection_timestamp_lsw +
                        ((uint64_t)detection_timestamp_msw<<32));
        } else {
            timestamps->first_stage_det_event_time = 1000 *
                ((uint64_t)det_ev_info->detection_timestamp_lsw +
                ((uint64_t)det_ev_info->detection_timestamp_msw << 32));
        }
    } else if (sm_info->type == PAL_SOUND_MODEL_TYPE_GENERIC) {
        event_size = sizeof(struct pal_st_generic_recognition_event);
        generic_event = (struct pal_st_generic_recognition_event *)
                       calloc(1, event_size);
        if (!generic_event) {
            PAL_ERR(LOG_TAG, "Failed to alloc memory for recognition event");
            status =  -ENOMEM;
            goto exit;
        }

        *event = &(generic_event->common);
        (*event)->status = PAL_RECOGNITION_STATUS_SUCCESS;
        (*event)->type = sm_info->type;
        (*event)->st_handle = (pal_st_handle_t *)this;
        (*event)->capture_available = sm_info->rec_config->capture_requested;
        // TODO: generate capture session
        (*event)->capture_session = 0;
        (*event)->capture_delay_ms = 0;
        (*event)->capture_preamble_ms = 0;
        (*event)->trigger_in_data = true;
        (*event)->data_size = 0;
        (*event)->data_offset = sizeof(struct pal_st_generic_recognition_event);
        (*event)->media_config.sample_rate =
            strAttr.in_media_config.sample_rate;
        (*event)->media_config.bit_width =
            strAttr.in_media_config.bit_width;
        (*event)->media_config.ch_info.channels =
            strAttr.in_media_config.ch_info.channels;
        (*event)->media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    }
    *size = event_size;
exit:
    PAL_DBG(LOG_TAG, "Exit");
    return status;
}

// Protected APIs
int32_t SVAInterface::ParseOpaqueConfLevels(
    struct sound_model_info *info,
    void *opaque_conf_levels,
    uint32_t version,
    uint8_t **out_conf_levels,
    uint32_t *out_num_conf_levels) {

    int32_t status = 0;
    struct st_confidence_levels_info *conf_levels = nullptr;
    struct st_confidence_levels_info_v2 *conf_levels_v2 = nullptr;
    struct st_sound_model_conf_levels *sm_levels = nullptr;
    struct st_sound_model_conf_levels_v2 *sm_levels_v2 = nullptr;
    int32_t confidence_level = 0;
    int32_t confidence_level_v2 = 0;
    bool gmm_conf_found = false;

    PAL_DBG(LOG_TAG, "Enter");
    if (version != CONF_LEVELS_INTF_VERSION_0002) {
        conf_levels = (struct st_confidence_levels_info *)
            ((char *)opaque_conf_levels + sizeof(struct st_param_header));

        st_conf_levels_ = (struct st_confidence_levels_info *)realloc(st_conf_levels_,
                sizeof(struct st_confidence_levels_info));
        if (!st_conf_levels_) {
            PAL_ERR(LOG_TAG, "failed to alloc stream conf_levels_");
            status = -ENOMEM;
            goto exit;
        }

        /* Cache to use during detection event processing */
        ar_mem_cpy((uint8_t *)st_conf_levels_, sizeof(struct st_confidence_levels_info),
            (uint8_t *)conf_levels, sizeof(struct st_confidence_levels_info));

        for (int i = 0; i < conf_levels->num_sound_models; i++) {
            sm_levels = &conf_levels->conf_levels[i];
            if (sm_levels->sm_id == ST_SM_ID_SVA_F_STAGE_GMM) {
                gmm_conf_found = true;
                status = FillOpaqueConfLevels(info->model_id, (void *)sm_levels,
                    out_conf_levels, out_num_conf_levels, version);
            } else if (sm_levels->sm_id & ST_SM_ID_SVA_S_STAGE_KWD ||
                       sm_levels->sm_id & ST_SM_ID_SVA_S_STAGE_USER) {
                confidence_level =
                    (sm_levels->sm_id & ST_SM_ID_SVA_S_STAGE_KWD) ?
                    sm_levels->kw_levels[0].kw_level:
                    sm_levels->kw_levels[0].user_levels[0].level;
                if (sm_levels->sm_id & ST_SM_ID_SVA_S_STAGE_KWD) {
                    PAL_INFO(LOG_TAG, "second stage keyword confidence level = %d",
                        confidence_level);
                } else {
                    PAL_INFO(LOG_TAG, "second stage user confidence level = %d",
                        confidence_level);
                }
                info->sec_threshold.push_back(
                    std::make_pair(sm_levels->sm_id, confidence_level));
            }
        }
    } else {
        conf_levels_v2 = (struct st_confidence_levels_info_v2 *)
            ((char *)opaque_conf_levels + sizeof(struct st_param_header));

        st_conf_levels_v2_ = (struct st_confidence_levels_info_v2 *)realloc(st_conf_levels_v2_,
            sizeof(struct st_confidence_levels_info_v2));
        if (!st_conf_levels_v2_) {
            PAL_ERR(LOG_TAG, "failed to alloc stream conf_levels_");
            status = -ENOMEM;
            goto exit;
        }

        /* Cache to use during detection event processing */
        ar_mem_cpy((uint8_t *)st_conf_levels_v2_, sizeof(struct st_confidence_levels_info_v2),
            (uint8_t *)conf_levels_v2, sizeof(struct st_confidence_levels_info_v2));

        for (int i = 0; i < conf_levels_v2->num_sound_models; i++) {
            sm_levels_v2 = &conf_levels_v2->conf_levels[i];
            if (sm_levels_v2->sm_id == ST_SM_ID_SVA_F_STAGE_GMM) {
                gmm_conf_found = true;
                status = FillOpaqueConfLevels(info->model_id, (void *)sm_levels_v2,
                    out_conf_levels, out_num_conf_levels, version);
            } else if (sm_levels_v2->sm_id & ST_SM_ID_SVA_S_STAGE_KWD ||
                       sm_levels_v2->sm_id & ST_SM_ID_SVA_S_STAGE_USER) {
                confidence_level_v2 =
                    (sm_levels_v2->sm_id & ST_SM_ID_SVA_S_STAGE_KWD) ?
                    sm_levels_v2->kw_levels[0].kw_level:
                    sm_levels_v2->kw_levels[0].user_levels[0].level;
                if (sm_levels_v2->sm_id & ST_SM_ID_SVA_S_STAGE_KWD) {
                    PAL_INFO(LOG_TAG, "second stage keyword confidence level = %d",
                        confidence_level_v2);
                } else {
                    PAL_INFO(LOG_TAG, "second stage user confidence level = %d",
                        confidence_level_v2);
                }
                info->sec_threshold.push_back(
                    std::make_pair(sm_levels_v2->sm_id, confidence_level_v2));
            }
        }
    }

    if (!gmm_conf_found || status) {
        PAL_ERR(LOG_TAG, "Did not receive GMM confidence threshold, error!");
        status = -EINVAL;
    }

exit:
    PAL_DBG(LOG_TAG, "Exit");

    return status;
}

int32_t SVAInterface::FillConfLevels(
    struct sound_model_info *info,
    struct pal_st_recognition_config *config,
    uint8_t **out_conf_levels,
    uint32_t *out_num_conf_levels) {

    int32_t status = 0;
    uint32_t num_conf_levels = 0;
    unsigned int user_level, user_id;
    unsigned int i = 0, j = 0;
    uint8_t *conf_levels = nullptr;
    unsigned char *user_id_tracker = nullptr;
    struct pal_st_phrase_sound_model *phrase_sm = nullptr;

    PAL_DBG(LOG_TAG, "Enter");

    if (!config) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "invalid input status %d", status);
        goto exit;
    }

    phrase_sm = (struct pal_st_phrase_sound_model *)info->model;

    if ((config->num_phrases == 0) ||
        (phrase_sm && config->num_phrases > phrase_sm->num_phrases)) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid phrase data status %d", status);
        goto exit;
    }

    for (i = 0; i < config->num_phrases; i++) {
        num_conf_levels++;
        if (info->model_id == 0) {
            for (j = 0; j < config->phrases[i].num_levels; j++)
                num_conf_levels++;
        }
    }

    conf_levels = (unsigned char*)calloc(1, num_conf_levels);
    if (!conf_levels) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "conf_levels calloc failed, status %d", status);
        goto exit;
    }

    user_id_tracker = (unsigned char *)calloc(1, num_conf_levels);
    if (!user_id_tracker) {
        status = -ENOMEM;
        PAL_ERR(LOG_TAG, "failed to allocate user_id_tracker status %d",
                status);
        goto exit;
    }

    for (i = 0; i < config->num_phrases; i++) {
        PAL_VERBOSE(LOG_TAG, "[%d] kw level %d", i,
        config->phrases[i].confidence_level);
        if (config->phrases[i].confidence_level > ST_MAX_FSTAGE_CONF_LEVEL) {
            PAL_ERR(LOG_TAG, "Invalid kw level %d",
                config->phrases[i].confidence_level);
            status = -EINVAL;
            goto exit;
        }
        for (j = 0; j < config->phrases[i].num_levels; j++) {
            PAL_VERBOSE(LOG_TAG, "[%d] user_id %d level %d ", i,
                        config->phrases[i].levels[j].user_id,
                        config->phrases[i].levels[j].level);
            if (config->phrases[i].levels[j].level > ST_MAX_FSTAGE_CONF_LEVEL) {
                PAL_ERR(LOG_TAG, "Invalid user level %d",
                    config->phrases[i].levels[j].level);
                status = -EINVAL;
                goto exit;
            }
        }
    }

    /* Example: Say the recognition structure has 3 keywords with users
     *      [0] k1 |uid|
     *              [0] u1 - 1st trainer
     *              [1] u2 - 4th trainer
     *              [3] u3 - 3rd trainer
     *      [1] k2
     *              [2] u2 - 2nd trainer
     *              [4] u3 - 5th trainer
     *      [2] k3
     *              [5] u4 - 6th trainer
     *    Output confidence level array will be
     *    [k1, k2, k3, u1k1, u2k1, u2k2, u3k1, u3k2, u4k3]
     */

    for (i = 0; i < config->num_phrases; i++) {
        conf_levels[i] = config->phrases[i].confidence_level;
        if (info->model_id == 0) {
            for (j = 0; j < config->phrases[i].num_levels; j++) {
                user_level = config->phrases[i].levels[j].level;
                user_id = config->phrases[i].levels[j].user_id;
                if ((user_id < config->num_phrases) ||
                     (user_id >= num_conf_levels)) {
                    status = -EINVAL;
                    PAL_ERR(LOG_TAG, "Invalid params user id %d status %d",
                            user_id, status);
                    goto exit;
                } else {
                    if (user_id_tracker[user_id] == 1) {
                        status = -EINVAL;
                        PAL_ERR(LOG_TAG, "Duplicate user id %d status %d", user_id,
                                status);
                        goto exit;
                    }
                    conf_levels[user_id] = (user_level < ST_MAX_FSTAGE_CONF_LEVEL) ?
                        user_level : ST_MAX_FSTAGE_CONF_LEVEL;
                    user_id_tracker[user_id] = 1;
                    PAL_VERBOSE(LOG_TAG, "user_conf_levels[%d] = %d", user_id,
                                conf_levels[user_id]);
                }
            }
        }
    }

    *out_conf_levels = conf_levels;
    *out_num_conf_levels = num_conf_levels;

exit:
    if (status && conf_levels) {
        free(conf_levels);
        *out_conf_levels = nullptr;
        *out_num_conf_levels = 0;
    }

    if (user_id_tracker)
        free(user_id_tracker);

    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SVAInterface::FillOpaqueConfLevels(
    uint32_t model_id,
    const void *sm_levels_generic,
    uint8_t **out_payload,
    uint32_t *out_payload_size,
    uint32_t version) {

    int status = 0;
    int32_t level = 0;
    unsigned int num_conf_levels = 0;
    unsigned int user_level = 0, user_id = 0;
    unsigned char *conf_levels = nullptr;
    unsigned int i = 0, j = 0;
    unsigned char *user_id_tracker = nullptr;
    struct st_sound_model_conf_levels *sm_levels = nullptr;
    struct st_sound_model_conf_levels_v2 *sm_levels_v2 = nullptr;

    PAL_VERBOSE(LOG_TAG, "Enter");

    /*  Example: Say the recognition structure has 3 keywords with users
     *  |kid|
     *  [0] k1 |uid|
     *         [3] u1 - 1st trainer
     *         [4] u2 - 4th trainer
     *         [6] u3 - 3rd trainer
     *  [1] k2
     *         [5] u2 - 2nd trainer
     *         [7] u3 - 5th trainer
     *  [2] k3
     *         [8] u4 - 6th trainer
     *
     *  Output confidence level array will be
     *  [k1, k2, k3, u1k1, u2k1, u2k2, u3k1, u3k2, u4k3]
     */

    if (version != CONF_LEVELS_INTF_VERSION_0002) {
        sm_levels = (struct st_sound_model_conf_levels *)sm_levels_generic;
        if (!sm_levels) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "ERROR. Invalid inputs");
            goto exit;
        }

        for (i = 0; i < sm_levels->num_kw_levels; i++) {
            level = sm_levels->kw_levels[i].kw_level;
            if (level < 0 || level > ST_MAX_FSTAGE_CONF_LEVEL) {
                PAL_ERR(LOG_TAG, "Invalid First stage [%d] kw level %d", i, level);
                status = -EINVAL;
                goto exit;
            } else {
                PAL_DBG(LOG_TAG, "First stage [%d] kw level %d", i, level);
            }
            for (j = 0; j < sm_levels->kw_levels[i].num_user_levels; j++) {
                level = sm_levels->kw_levels[i].user_levels[j].level;
                if (level < 0 || level > ST_MAX_FSTAGE_CONF_LEVEL) {
                    PAL_ERR(LOG_TAG, "Invalid First stage [%d] user_id %d level %d", i,
                        sm_levels->kw_levels[i].user_levels[j].user_id, level);
                    status = -EINVAL;
                    goto exit;
                } else {
                    PAL_DBG(LOG_TAG, "First stage [%d] user_id %d level %d ", i,
                        sm_levels->kw_levels[i].user_levels[j].user_id, level);
                }
            }
        }

        for (i = 0; i < sm_levels->num_kw_levels; i++) {
            num_conf_levels++;
            if (model_id == 0) {
                for (j = 0; j < sm_levels->kw_levels[i].num_user_levels; j++)
                    num_conf_levels++;
            }
        }

        PAL_DBG(LOG_TAG, "Number of confidence levels : %d", num_conf_levels);

        if (!num_conf_levels) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "ERROR. Invalid num_conf_levels input");
            goto exit;
        }

        conf_levels = (unsigned char*)calloc(1, num_conf_levels);
        if (!conf_levels) {
            status = -ENOMEM;
            PAL_ERR(LOG_TAG, "conf_levels calloc failed, status %d", status);
            goto exit;
        }

        user_id_tracker = (unsigned char *)calloc(1, num_conf_levels);
        if (!user_id_tracker) {
            status = -ENOMEM;
            PAL_ERR(LOG_TAG, "failed to allocate user_id_tracker status %d",
                    status);
            goto exit;
        }

        for (i = 0; i < sm_levels->num_kw_levels; i++) {
            if (i < num_conf_levels) {
                conf_levels[i] = sm_levels->kw_levels[i].kw_level;
            } else {
                status = -EINVAL;
                PAL_ERR(LOG_TAG, "ERROR. Invalid numver of kw levels");
                goto exit;
            }
            if (model_id == 0) {
                for (j = 0; j < sm_levels->kw_levels[i].num_user_levels; j++) {
                    user_level = sm_levels->kw_levels[i].user_levels[j].level;
                    user_id = sm_levels->kw_levels[i].user_levels[j].user_id;
                    if ((user_id < sm_levels->num_kw_levels) ||
                        (user_id >= num_conf_levels)) {
                        status = -EINVAL;
                        PAL_ERR(LOG_TAG, "ERROR. Invalid params user id %d>%d",
                                user_id, num_conf_levels);
                        goto exit;
                    } else {
                        if (user_id_tracker[user_id] == 1) {
                            status = -EINVAL;
                            PAL_ERR(LOG_TAG, "ERROR. Duplicate user id %d",
                                    user_id);
                            goto exit;
                        }
                        conf_levels[user_id] = user_level;
                        user_id_tracker[user_id] = 1;
                        PAL_ERR(LOG_TAG, "user_conf_levels[%d] = %d",
                                user_id, conf_levels[user_id]);
                    }
                }
            }
        }
    } else {
        sm_levels_v2 =
            (struct st_sound_model_conf_levels_v2 *)sm_levels_generic;
        if (!sm_levels_v2) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "ERROR. Invalid inputs");
            goto exit;
        }

        for (i = 0; i < sm_levels_v2->num_kw_levels; i++) {
            level = sm_levels_v2->kw_levels[i].kw_level;
            if (level < 0 || level > ST_MAX_FSTAGE_CONF_LEVEL) {
                PAL_ERR(LOG_TAG, "Invalid First stage [%d] kw level %d", i, level);
                status = -EINVAL;
                goto exit;
            } else {
                PAL_DBG(LOG_TAG, "First stage [%d] kw level %d", i, level);
            }
            for (j = 0; j < sm_levels_v2->kw_levels[i].num_user_levels; j++) {
                level = sm_levels_v2->kw_levels[i].user_levels[j].level;
                if (level < 0 || level > ST_MAX_FSTAGE_CONF_LEVEL) {
                    PAL_ERR(LOG_TAG, "Invalid First stage [%d] user_id %d level %d", i,
                        sm_levels_v2->kw_levels[i].user_levels[j].user_id, level);
                    status = -EINVAL;
                    goto exit;
                } else {
                    PAL_DBG(LOG_TAG, "First stage [%d] user_id %d level %d ", i,
                        sm_levels_v2->kw_levels[i].user_levels[j].user_id, level);
                }
            }
        }

        for (i = 0; i < sm_levels_v2->num_kw_levels; i++) {
            num_conf_levels++;
            if (model_id == 0) {
                for (j = 0; j < sm_levels_v2->kw_levels[i].num_user_levels; j++)
                    num_conf_levels++;
            }
        }

        PAL_DBG(LOG_TAG,"number of confidence levels : %d", num_conf_levels);

        if (!num_conf_levels) {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "ERROR. Invalid num_conf_levels input");
            goto exit;
        }

        conf_levels = (unsigned char*)calloc(1, num_conf_levels);
        if (!conf_levels) {
            status = -ENOMEM;
            PAL_ERR(LOG_TAG, "conf_levels calloc failed, status %d", status);
            goto exit;
        }

        user_id_tracker = (unsigned char *)calloc(1, num_conf_levels);
        if (!user_id_tracker) {
            status = -ENOMEM;
            PAL_ERR(LOG_TAG, "failed to allocate user_id_tracker status %d",
                    status);
            goto exit;
        }

        for (i = 0; i < sm_levels_v2->num_kw_levels; i++) {
            if (i < num_conf_levels) {
                conf_levels[i] = sm_levels_v2->kw_levels[i].kw_level;
            } else {
                status = -EINVAL;
                PAL_ERR(LOG_TAG, "ERROR. Invalid numver of kw levels");
                goto exit;
            }
            if (model_id == 0) {
                for (j = 0; j < sm_levels_v2->kw_levels[i].num_user_levels; j++) {
                    user_level = sm_levels_v2->kw_levels[i].user_levels[j].level;
                    user_id = sm_levels_v2->kw_levels[i].user_levels[j].user_id;
                    if ((user_id < sm_levels_v2->num_kw_levels) ||
                         (user_id >= num_conf_levels)) {
                        status = -EINVAL;
                        PAL_ERR(LOG_TAG, "ERROR. Invalid params user id %d>%d",
                              user_id, num_conf_levels);
                        goto exit;
                    } else {
                        if (user_id_tracker[user_id] == 1) {
                            status = -EINVAL;
                            PAL_ERR(LOG_TAG, "ERROR. Duplicate user id %d",
                                user_id);
                            goto exit;
                        }
                        conf_levels[user_id] = user_level;
                        user_id_tracker[user_id] = 1;
                        PAL_VERBOSE(LOG_TAG, "user_conf_levels[%d] = %d",
                        user_id, conf_levels[user_id]);
                    }
                }
            }
        }
    }

    *out_payload = conf_levels;
    *out_payload_size = num_conf_levels;
    PAL_DBG(LOG_TAG, "Returning number of conf levels : %d", *out_payload_size);
exit:
    if (status && conf_levels) {
        free(conf_levels);
        *out_payload = nullptr;
        *out_payload_size = 0;
    }

    if (user_id_tracker)
        free(user_id_tracker);

    PAL_DBG(LOG_TAG, "Exit, status %d", status);
    return status;
}

int32_t SVAInterface::ParseDetectionPayloadPDK(void *event_data) {
    int32_t status = 0;
    uint32_t payload_size = 0;
    uint32_t parsed_size = 0;
    uint32_t event_size = 0;
    uint32_t keyId = 0;
    uint64_t kwd_start_timestamp = 0;
    uint64_t kwd_end_timestamp = 0;
    uint64_t ftrt_start_timestamp = 0;
    uint8_t *ptr = nullptr;
    struct event_id_detection_engine_generic_info_t *generic_info = nullptr;
    struct detection_event_info_header_t *event_header = nullptr;
    struct ftrt_data_info_t *ftrt_info = nullptr;
    struct voice_ui_multi_model_result_info_t *multi_model_result = nullptr;
    struct model_stats *model_stat = nullptr;
    struct model_stats *detected_model_stat = nullptr;

    PAL_DBG(LOG_TAG, "Enter");
    if (!event_data) {
        PAL_ERR(LOG_TAG, "Invalid event data");
        return -EINVAL;
    }

    std::memset(&detection_event_info_multi_model_, 0,
            sizeof(struct voice_ui_multi_model_result_info_t));

    generic_info = (struct event_id_detection_engine_generic_info_t *)
                    event_data;
    payload_size = sizeof(struct event_id_detection_engine_generic_info_t);
    event_size = generic_info->payload_size;
    ptr = (uint8_t *)event_data + payload_size;

    if (!event_size) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid detection payload");
        goto exit;
    }

    PAL_INFO(LOG_TAG, "event_size = %u", event_size);

    while (parsed_size < event_size) {
        PAL_DBG(LOG_TAG, "parsed_size = %u, event_size = %u", parsed_size,
                                                              event_size);
        event_header = (struct detection_event_info_header_t *)ptr;
        keyId = event_header->key_id;
        payload_size = event_header->payload_size;
        ptr += sizeof(struct detection_event_info_header_t);
        parsed_size += sizeof(struct detection_event_info_header_t);

        switch (keyId) {
            case KEY_ID_FTRT_DATA_INFO :
                PAL_INFO(LOG_TAG, "ftrt structure size : %u", payload_size);

                ftrt_info = (struct ftrt_data_info_t *)ptr;
                detection_event_info_multi_model_.ftrt_data_length_in_us =
                                        ftrt_info->ftrt_data_length_in_us;
                PAL_INFO(LOG_TAG, "ftrt_data_length_in_us = %u",
                detection_event_info_multi_model_.ftrt_data_length_in_us);
                ftrt_size_ = UsToBytes(ftrt_info->ftrt_data_length_in_us);
                break;

            case KEY_ID_VOICE_UI_MULTI_MODEL_RESULT_INFO :
                PAL_INFO(LOG_TAG, "voice_ui_multi_model_result_info : %u",
                        payload_size );

                multi_model_result = (struct voice_ui_multi_model_result_info_t *)
                                      ptr;
                detection_event_info_multi_model_.num_detected_models =
                                 multi_model_result->num_detected_models;
                PAL_INFO(LOG_TAG, "Number of detected models : %d",
                detection_event_info_multi_model_.num_detected_models);

                model_stat = (struct model_stats *)(ptr +
                             sizeof(struct voice_ui_multi_model_result_info_t));
                det_model_id_ = model_stat->detected_model_id;
                for (int i = 0; i < detection_event_info_multi_model_.
                                    num_detected_models; ++i) {

                    detection_event_info_multi_model_.detected_model_stats[i].
                    detected_model_id = model_stat->detected_model_id;

                    detection_event_info_multi_model_.detected_model_stats[i].
                    detected_keyword_id = model_stat->detected_keyword_id;
                    PAL_INFO(LOG_TAG, "detected keyword id : %u",
                            detection_event_info_multi_model_.detected_model_stats[i].
                            detected_keyword_id);

                    detection_event_info_multi_model_.detected_model_stats[i].
                    best_channel_idx = model_stat->best_channel_idx;

                    detection_event_info_multi_model_.detected_model_stats[i].
                    best_confidence_level = model_stat->best_confidence_level;
                    PAL_INFO(LOG_TAG, "detected best conf level : %u",
                            detection_event_info_multi_model_.detected_model_stats[i].
                            best_confidence_level);

                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_start_timestamp_lsw = model_stat->kw_start_timestamp_lsw;
                    PAL_INFO(LOG_TAG, "kw_start_timestamp_lsw : %u",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_start_timestamp_lsw);

                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_start_timestamp_msw = model_stat->kw_start_timestamp_msw;
                    PAL_INFO(LOG_TAG, "kw_start_timestamp_msw : %u",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_start_timestamp_msw);

                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_end_timestamp_lsw = model_stat->kw_end_timestamp_lsw;
                    PAL_INFO(LOG_TAG, "kw_end_timestamp_lsw : %u",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_end_timestamp_lsw);


                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_end_timestamp_msw = model_stat->kw_end_timestamp_msw;
                    PAL_INFO(LOG_TAG, "kw_end_timestamp_msw : %u",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    kw_end_timestamp_msw);


                    detection_event_info_multi_model_.detected_model_stats[i].
                    detection_timestamp_lsw = model_stat->detection_timestamp_lsw;
                    PAL_INFO(LOG_TAG, "detection_timestamp_lsw : %u",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    detection_timestamp_lsw);

                    detection_event_info_multi_model_.detected_model_stats[i].
                    detection_timestamp_msw = model_stat->detection_timestamp_msw;
                    PAL_INFO(LOG_TAG, "detection_timestamp_msw : %u",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    detection_timestamp_msw);

                    PAL_INFO(LOG_TAG," Detection made for model id : %x",
                    detection_event_info_multi_model_.detected_model_stats[i].
                    detected_model_id);
                    model_stat += sizeof(struct model_stats);
                }
                break;
            default :
                status = -EINVAL;
                PAL_ERR(LOG_TAG, "Invalid key id %u status %d", keyId, status);
                goto exit;
        }
        ptr += payload_size;
        parsed_size += payload_size;

    }

    detected_model_stat =
        &detection_event_info_multi_model_.detected_model_stats[0];

    kwd_start_timestamp =
        (uint64_t)detected_model_stat->kw_start_timestamp_lsw +
        ((uint64_t)detected_model_stat->kw_start_timestamp_msw << 32);
    kwd_end_timestamp =
        (uint64_t)detected_model_stat->kw_end_timestamp_lsw +
        ((uint64_t)detected_model_stat->kw_end_timestamp_msw << 32);
    ftrt_start_timestamp =
        (uint64_t)detected_model_stat->detection_timestamp_lsw +
        ((uint64_t)detected_model_stat->detection_timestamp_msw << 32) -
        detection_event_info_multi_model_.ftrt_data_length_in_us;

    UpdateKeywordIndex(kwd_start_timestamp, kwd_end_timestamp,
        ftrt_start_timestamp);

exit :
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

int32_t SVAInterface::ParseDetectionPayloadGMM(void *event_data) {
    int32_t status = 0;
    int32_t i = 0;
    uint32_t parsed_size = 0;
    uint32_t payload_size = 0;
    uint32_t event_size = 0;
    uint64_t kwd_start_timestamp = 0;
    uint64_t kwd_end_timestamp = 0;
    uint64_t ftrt_start_timestamp = 0;
    uint8_t *ptr = nullptr;
    struct event_id_detection_engine_generic_info_t *generic_info = nullptr;
    struct detection_event_info_header_t *event_header = nullptr;
    struct confidence_level_info_t *confidence_info = nullptr;
    struct keyword_position_info_t *keyword_position_info = nullptr;
    struct detection_timestamp_info_t *detection_timestamp_info = nullptr;
    struct ftrt_data_info_t *ftrt_info = nullptr;

    PAL_DBG(LOG_TAG, "Enter");
    if (!event_data) {
        PAL_ERR(LOG_TAG, "Invalid event data");
        return -EINVAL;
    }

    std::memset(&detection_event_info_, 0, sizeof(struct detection_event_info));

    generic_info =
        (struct event_id_detection_engine_generic_info_t *)event_data;
    payload_size = sizeof(struct event_id_detection_engine_generic_info_t);
    detection_event_info_.status = generic_info->status;
    event_size = generic_info->payload_size;
    ptr = (uint8_t *)event_data + payload_size;
    PAL_INFO(LOG_TAG, "status = %u, event_size = %u",
                detection_event_info_.status, event_size);
    if (status || !event_size) {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid detection payload");
        goto exit;
    }

    // parse variable payload
    while (parsed_size < event_size) {
        PAL_DBG(LOG_TAG, "parsed_size = %u, event_size = %u",
                parsed_size, event_size);
        event_header = (struct detection_event_info_header_t *)ptr;
        uint32_t keyId = event_header->key_id;
        payload_size = event_header->payload_size;
        PAL_DBG(LOG_TAG, "key id = %u, payload_size = %u",
                keyId, payload_size);
        ptr += sizeof(struct detection_event_info_header_t);
        parsed_size += sizeof(struct detection_event_info_header_t);

        switch (keyId) {
        case KEY_ID_CONFIDENCE_LEVELS_INFO:
            confidence_info = (struct confidence_level_info_t *)ptr;
            detection_event_info_.num_confidence_levels =
                confidence_info->number_of_confidence_values;
            PAL_INFO(LOG_TAG, "num_confidence_levels = %u",
                    detection_event_info_.num_confidence_levels);
            for (i = 0; i < detection_event_info_.num_confidence_levels; i++) {
                detection_event_info_.confidence_levels[i] =
                    confidence_info->confidence_levels[i];
                PAL_INFO(LOG_TAG, "confidence_levels[%d] = %u", i,
                        detection_event_info_.confidence_levels[i]);
            }
            break;
        case KEY_ID_KWD_POSITION_INFO:
            keyword_position_info = (struct keyword_position_info_t *)ptr;
            detection_event_info_.kw_start_timestamp_lsw =
                keyword_position_info->kw_start_timestamp_lsw;
            detection_event_info_.kw_start_timestamp_msw =
                keyword_position_info->kw_start_timestamp_msw;
            detection_event_info_.kw_end_timestamp_lsw =
                keyword_position_info->kw_end_timestamp_lsw;
            detection_event_info_.kw_end_timestamp_msw =
                keyword_position_info->kw_end_timestamp_msw;
            PAL_INFO(LOG_TAG, "start_lsw = %u, start_msw = %u, "
                    "end_lsw = %u, end_msw = %u",
                    detection_event_info_.kw_start_timestamp_lsw,
                    detection_event_info_.kw_start_timestamp_msw,
                    detection_event_info_.kw_end_timestamp_lsw,
                    detection_event_info_.kw_end_timestamp_msw);
            break;
        case KEY_ID_TIMESTAMP_INFO:
            detection_timestamp_info = (struct detection_timestamp_info_t *)ptr;
            detection_event_info_.detection_timestamp_lsw =
                detection_timestamp_info->detection_timestamp_lsw;
            detection_event_info_.detection_timestamp_msw =
                detection_timestamp_info->detection_timestamp_msw;
            PAL_INFO(LOG_TAG, "timestamp_lsw = %u, timestamp_msw = %u",
                    detection_event_info_.detection_timestamp_lsw,
                    detection_event_info_.detection_timestamp_msw);
            break;
        case KEY_ID_FTRT_DATA_INFO:
            ftrt_info = (struct ftrt_data_info_t *)ptr;
            ftrt_size_ = UsToBytes(ftrt_info->ftrt_data_length_in_us);
            detection_event_info_.ftrt_data_length_in_us =
                ftrt_info->ftrt_data_length_in_us;
            PAL_INFO(LOG_TAG, "ftrt_data_length_in_us = %u",
                    detection_event_info_.ftrt_data_length_in_us);
            break;
        default:
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid key id %u status %d", keyId, status);
            goto exit;
        }
        ptr += payload_size;
        parsed_size += payload_size;
    }

    kwd_start_timestamp =
        (uint64_t)detection_event_info_.kw_start_timestamp_lsw +
        ((uint64_t)detection_event_info_.kw_start_timestamp_msw << 32);
    kwd_end_timestamp =
        (uint64_t)detection_event_info_.kw_end_timestamp_lsw +
        ((uint64_t)detection_event_info_.kw_end_timestamp_msw << 32);
    ftrt_start_timestamp =
        (uint64_t)detection_event_info_.detection_timestamp_lsw +
        ((uint64_t)detection_event_info_.detection_timestamp_msw << 32) -
        detection_event_info_.ftrt_data_length_in_us;

    UpdateKeywordIndex(kwd_start_timestamp, kwd_end_timestamp,
        ftrt_start_timestamp);
    det_model_id_ = 0;
exit:
    PAL_DBG(LOG_TAG, "Exit, status %d", status);

    return status;
}

void SVAInterface::UpdateKeywordIndex(uint64_t kwd_start_timestamp,
                                      uint64_t kwd_end_timestamp,
                                      uint64_t ftrt_start_timestamp) {

    PAL_VERBOSE(LOG_TAG, "kwd start timestamp: %llu, kwd end timestamp: %llu",
        (long long)kwd_start_timestamp, (long long)kwd_end_timestamp);
    PAL_VERBOSE(LOG_TAG, "Ftrt data start timestamp : %llu",
        (long long)ftrt_start_timestamp);

    if (kwd_start_timestamp >= kwd_end_timestamp ||
        kwd_start_timestamp < ftrt_start_timestamp) {
        PAL_DBG(LOG_TAG, "Invalid timestamp, cannot compute keyword index");
        return;
    }

    start_index_ = UsToBytes(kwd_start_timestamp - ftrt_start_timestamp);
    end_index_ = UsToBytes(kwd_end_timestamp - ftrt_start_timestamp);
    PAL_INFO(LOG_TAG, "start_index : %zu, end_index : %zu",
        start_index_, end_index_);
}

void SVAInterface::PackEventConfLevels(struct sound_model_info *sm_info,
                                       uint8_t *opaque_data) {

    struct st_confidence_levels_info *conf_levels = nullptr;
    struct st_confidence_levels_info_v2 *conf_levels_v2 = nullptr;
    uint32_t i = 0, j = 0, k = 0, user_id = 0, num_user_levels = 0;

    PAL_VERBOSE(LOG_TAG, "Enter");

    /*
     * Update the opaque data of callback event with confidence levels
     * accordingly for all users and keywords from the detection event
     */
    if (conf_levels_intf_version_ != CONF_LEVELS_INTF_VERSION_0002) {
        conf_levels = (struct st_confidence_levels_info *)opaque_data;
        for (i = 0; i < conf_levels->num_sound_models; i++) {
            if (conf_levels->conf_levels[i].sm_id == ST_SM_ID_SVA_F_STAGE_GMM) {
                for (j = 0; j < conf_levels->conf_levels[i].num_kw_levels; j++) {
                    if (j <= sm_info->info->GetConfLevelsSize())
                        conf_levels->conf_levels[i].kw_levels[j].kw_level =
                            sm_info->info->GetDetConfLevels()[j];
                    else
                        PAL_ERR(LOG_TAG, "unexpected conf size %d < %d",
                            sm_info->info->GetConfLevelsSize(), j);

                    num_user_levels =
                        conf_levels->conf_levels[i].kw_levels[j].num_user_levels;
                    for (k = 0; k < num_user_levels; k++) {
                        user_id = conf_levels->conf_levels[i].kw_levels[j].
                            user_levels[k].user_id;
                        if (user_id <= sm_info->info->GetConfLevelsSize())
                            conf_levels->conf_levels[i].kw_levels[j].user_levels[k].
                                level = sm_info->info->GetDetConfLevels()[user_id];
                        else
                            PAL_ERR(LOG_TAG, "Unexpected conf size %d < %d",
                                sm_info->info->GetConfLevelsSize(), user_id);
                    }
                }
            } else if (conf_levels->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_KWD ||
                       conf_levels->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_USER) {
                /* Update confidence levels for second stage */
                for (auto &iter: sm_info->sec_det_level) {
                    if ((conf_levels->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_KWD) &&
                        (iter.first & ST_SM_ID_SVA_S_STAGE_KWD)) {
                        conf_levels->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels->conf_levels[i].kw_levels[0].user_levels[0].level = 0;
                    } else if ((conf_levels->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_USER) &&
                               (iter.first == conf_levels->conf_levels[i].sm_id)) {
                        conf_levels->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels->conf_levels[i].kw_levels[0].user_levels[0].level = iter.second;
                    }
                }
            }
        }
    } else {
        conf_levels_v2 = (struct st_confidence_levels_info_v2 *)opaque_data;
        for (i = 0; i < conf_levels_v2->num_sound_models; i++) {
            if (conf_levels_v2->conf_levels[i].sm_id == ST_SM_ID_SVA_F_STAGE_GMM) {
                for (j = 0; j < conf_levels_v2->conf_levels[i].num_kw_levels; j++) {
                    if (j <= sm_info->info->GetConfLevelsSize())
                            conf_levels_v2->conf_levels[i].kw_levels[j].kw_level =
                                    sm_info->info->GetDetConfLevels()[j];
                    else
                        PAL_ERR(LOG_TAG, "unexpected conf size %d < %d",
                            sm_info->info->GetConfLevelsSize(), j);

                    PAL_INFO(LOG_TAG, "First stage KW Conf levels[%d]-%d",
                        j, sm_info->info->GetDetConfLevels()[j]);

                    num_user_levels =
                        conf_levels_v2->conf_levels[i].kw_levels[j].num_user_levels;
                    for (k = 0; k < num_user_levels; k++) {
                        user_id = conf_levels_v2->conf_levels[i].kw_levels[j].
                            user_levels[k].user_id;
                        if (user_id <=  sm_info->info->GetConfLevelsSize())
                            conf_levels_v2->conf_levels[i].kw_levels[j].user_levels[k].
                                level = sm_info->info->GetDetConfLevels()[user_id];
                        else
                            PAL_ERR(LOG_TAG, "Unexpected conf size %d < %d",
                                sm_info->info->GetConfLevelsSize(), user_id);

                        PAL_INFO(LOG_TAG, "First stage User Conf levels[%d]-%d",
                            k, sm_info->info->GetDetConfLevels()[user_id]);
                    }
                }
            } else if (conf_levels_v2->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_KWD ||
                       conf_levels_v2->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_USER) {
                /* Update confidence levels for second stage */
                for (auto &iter: sm_info->sec_det_level) {
                    if ((conf_levels_v2->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_KWD) &&
                        (iter.first & ST_SM_ID_SVA_S_STAGE_KWD)) {
                        conf_levels_v2->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels_v2->conf_levels[i].kw_levels[0].user_levels[0].level = 0;
                    } else if ((conf_levels_v2->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_USER) &&
                               (iter.first == ST_SM_ID_SVA_S_STAGE_USER)) {
                        conf_levels_v2->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels_v2->conf_levels[i].kw_levels[0].user_levels[0].level = iter.second;
                    }
                }
            }
        }
    }
    PAL_VERBOSE(LOG_TAG, "Exit");
}

void SVAInterface::FillCallbackConfLevels(struct sound_model_info *sm_info,
                                          uint8_t *opaque_data,
                                          uint32_t det_keyword_id,
                                          uint32_t best_conf_level) {
    int i = 0;
    struct st_confidence_levels_info_v2 *conf_levels_v2 = nullptr;
    struct st_confidence_levels_info *conf_levels = nullptr;

    if (conf_levels_intf_version_ != CONF_LEVELS_INTF_VERSION_0002) {
        conf_levels = (struct st_confidence_levels_info *)opaque_data;
        for (i = 0; i < conf_levels->num_sound_models; i++) {
            if (conf_levels->conf_levels[i].sm_id == ST_SM_ID_SVA_F_STAGE_GMM) {
                conf_levels->conf_levels[i].kw_levels[det_keyword_id].
                    kw_level = best_conf_level;
                conf_levels->conf_levels[i].kw_levels[det_keyword_id].
                    user_levels[0].level = 0;
                PAL_INFO(LOG_TAG, "First stage returning conf level : %d",
                    best_conf_level);
            } else if (conf_levels->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_KWD) {
                for (auto iter: sm_info->sec_det_level) {
                    if (iter.first & ST_SM_ID_SVA_S_STAGE_KWD) {
                        conf_levels->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels->conf_levels[i].kw_levels[0].user_levels[0].level = 0;
                        PAL_INFO(LOG_TAG, "Second stage keyword conf level: %d", iter.second);
                    }
                }
            } else if (conf_levels->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_USER) {
                for (auto iter: sm_info->sec_det_level) {
                    if (iter.first & ST_SM_ID_SVA_S_STAGE_USER) {
                        conf_levels->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels->conf_levels[i].kw_levels[0].user_levels[0].level = iter.second;
                        PAL_INFO(LOG_TAG, "Second stage user conf level: %d", iter.second);
                    }
                }
            }
        }
    } else {
        conf_levels_v2 = (struct st_confidence_levels_info_v2 *)opaque_data;
        for (i = 0; i < conf_levels_v2->num_sound_models; i++) {
            if (conf_levels_v2->conf_levels[i].sm_id == ST_SM_ID_SVA_F_STAGE_GMM) {
                conf_levels_v2->conf_levels[i].kw_levels[det_keyword_id].
                    kw_level = best_conf_level;
                conf_levels_v2->conf_levels[i].kw_levels[det_keyword_id].
                    user_levels[0].level = 0;
                PAL_INFO(LOG_TAG, "First stage returning conf level: %d",
                    best_conf_level);
            } else if (conf_levels_v2->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_KWD) {
                for (auto iter: sm_info->sec_det_level) {
                    if (iter.first & ST_SM_ID_SVA_S_STAGE_KWD) {
                        conf_levels_v2->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels_v2->conf_levels[i].kw_levels[0].user_levels[0].level = 0;
                        PAL_INFO(LOG_TAG, "Second stage keyword conf level: %d", iter.second);
                    }
                }
            } else if (conf_levels_v2->conf_levels[i].sm_id & ST_SM_ID_SVA_S_STAGE_USER) {
                for (auto iter: sm_info->sec_det_level) {
                    if (iter.first & ST_SM_ID_SVA_S_STAGE_USER) {
                        conf_levels_v2->conf_levels[i].kw_levels[0].kw_level = iter.second;
                        conf_levels_v2->conf_levels[i].kw_levels[0].user_levels[0].level = iter.second;
                        PAL_INFO(LOG_TAG, "Second stage user conf level: %d", iter.second);
                    }
                }
            }
        }
    }
}

void SVAInterface::CheckAndSetDetectionConfLevels(Stream *s) {
    PAL_DBG(LOG_TAG, "Enter");

    if (!s) {
        PAL_ERR(LOG_TAG, "Invalid detected stream");
        return;
    }

    if (detection_event_info_.num_confidence_levels <
            sound_model_info_->GetConfLevelsSize()) {
        PAL_ERR(LOG_TAG, "detection event conf lvls %d < eng conf lvl size %d",
            detection_event_info_.num_confidence_levels,
            sound_model_info_->GetConfLevelsSize());
        return;
    }
    /* Reset any cached previous detection conf level values */
    sm_info_map_[s]->info->ResetDetConfLevels();

    /* Extract the stream conf level values from SPF detection payload */
    for (uint32_t i = 0; i < sound_model_info_->GetConfLevelsSize(); i++) {
        if (!detection_event_info_.confidence_levels[i])
            continue;
        for (uint32_t j = 0; j < sm_info_map_[s]->info->GetConfLevelsSize(); j++) {
            if (!strcmp(sm_info_map_[s]->info->GetConfLevelsKwUsers()[j],
                 sound_model_info_->GetConfLevelsKwUsers()[i])) {
                 sm_info_map_[s]->info->UpdateDetConfLevel(j,
                   detection_event_info_.confidence_levels[i]);
            }
        }
    }

    for (uint32_t i = 0; i < sm_info_map_[s]->info->GetConfLevelsSize(); i++)
        PAL_INFO(LOG_TAG, "det_cf_levels[%d]-%d", i,
            sm_info_map_[s]->info->GetDetConfLevels()[i]);
}

int32_t SVAInterface::QuerySoundModel(
    SoundModelInfo *sm_info,
    uint8_t *data,
    uint32_t data_size) {

    listen_sound_model_header sml_header = {};
    listen_model_type model = {};
    listen_status_enum sml_ret = kSucess;
    uint32_t status = 0;
    std::shared_ptr<SoundModelLib>sml = SoundModelLib::GetInstance();

    PAL_VERBOSE(LOG_TAG, "Enter: sound model size %d", data_size);

    if (!sml || !sm_info) {
        PAL_ERR(LOG_TAG, "soundmodel lib handle or model info NULL");
        return -ENOSYS;
    }

    model.data = data;
    model.size = data_size;

    sml_ret = sml->GetSoundModelHeader_(&model, &sml_header);
    if (sml_ret != kSucess) {
        PAL_ERR(LOG_TAG, "GetSoundModelHeader_ failed, err %d ", sml_ret);
        return -EINVAL;
    }
    if (sml_header.numKeywords == 0) {
        PAL_ERR(LOG_TAG, "num keywords zero!");
        return -EINVAL;
    }

    if (sml_header.numActiveUserKeywordPairs < sml_header.numUsers) {
        PAL_ERR(LOG_TAG, "smlib activeUserKwPairs(%d) < total users (%d)",
                sml_header.numActiveUserKeywordPairs, sml_header.numUsers);
        goto cleanup;
    }
    if (sml_header.numUsers && !sml_header.userKeywordPairFlags) {
        PAL_ERR(LOG_TAG, "userKeywordPairFlags is NULL, numUsers (%d)",
                sml_header.numUsers);
        goto cleanup;
    }

    PAL_VERBOSE(LOG_TAG, "SML model.data %pK, model.size %d", model.data,
            model.size);
    status = sm_info->SetKeyPhrases(&model, sml_header.numKeywords);
    if (status)
        goto cleanup;

    status = sm_info->SetUsers(&model, sml_header.numUsers);
    if (status)
        goto cleanup;

    status = sm_info->SetConfLevels(sml_header.numActiveUserKeywordPairs,
                                    sml_header.numUsersSetPerKw,
                                    sml_header.userKeywordPairFlags);
    if (status)
        goto cleanup;

    sml_ret = sml->ReleaseSoundModelHeader_(&sml_header);
    if (sml_ret != kSucess) {
        PAL_ERR(LOG_TAG, "ReleaseSoundModelHeader failed, err %d ", sml_ret);
        status = -EINVAL;
        goto cleanup_1;
    }
    PAL_VERBOSE(LOG_TAG, "exit");
    return 0;

cleanup:
    sml_ret = sml->ReleaseSoundModelHeader_(&sml_header);
    if (sml_ret != kSucess)
        PAL_ERR(LOG_TAG, "ReleaseSoundModelHeader_ failed, err %d ", sml_ret);

cleanup_1:
    return status;
}

int32_t SVAInterface::MergeSoundModels(
    uint32_t num_models,
    listen_model_type *in_models[],
    listen_model_type *out_model) {

    listen_status_enum sm_ret = kSucess;
    int32_t status = 0;
    std::shared_ptr<SoundModelLib>sml = SoundModelLib::GetInstance();

    if (!sml) {
        PAL_ERR(LOG_TAG, "soundmodel lib handle NULL");
        return -ENOSYS;
    }

    PAL_VERBOSE(LOG_TAG, "num_models to merge %d", num_models);
    sm_ret = sml->GetMergedModelSize_(num_models, in_models,
        &out_model->size);
    if ((sm_ret != kSucess) || !out_model->size) {
        PAL_ERR(LOG_TAG, "GetMergedModelSize failed, err %d, size %d",
            sm_ret, out_model->size);
        return -EINVAL;
    }
    PAL_INFO(LOG_TAG, "merged sound model size %d", out_model->size);

    out_model->data = (uint8_t *)calloc(1, out_model->size * sizeof(char));
    if (!out_model->data) {
        PAL_ERR(LOG_TAG, "Merged sound model allocation failed");
        return -ENOMEM;
    }

    sm_ret = sml->MergeModels_(num_models, in_models, out_model);
    if (sm_ret != kSucess) {
        PAL_ERR(LOG_TAG, "MergeModels failed, err %d", sm_ret);
        status = -EINVAL;
        goto cleanup;
    }
    if (!out_model->data || !out_model->size) {
        PAL_ERR(LOG_TAG, "MergeModels returned NULL data or size %d",
              out_model->size);
        status = -EINVAL;
        goto cleanup;
    }

    if (VoiceUIPlatformInfo::GetInstance()->GetEnableDebugDumps()) {
        ST_DBG_DECLARE(FILE *sm_fd = NULL;
            static int sm_cnt = 0);
        ST_DBG_FILE_OPEN_WR(sm_fd, ST_DEBUG_DUMP_LOCATION,
            "st_smlib_output_merged_sm", "bin", sm_cnt);
        ST_DBG_FILE_WRITE(sm_fd, out_model->data, out_model->size);
        ST_DBG_FILE_CLOSE(sm_fd);
        PAL_DBG(LOG_TAG, "SM returned from SML merge stored in: st_smlib_output_merged_sm_%d.bin",
            sm_cnt);
        sm_cnt++;
    }
    PAL_DBG(LOG_TAG, "Exit, status: %d", status);
    return 0;

cleanup:
    if (out_model->data) {
        free(out_model->data);
        out_model->data = nullptr;
        out_model->size = 0;
    }
    return status;
}

int32_t SVAInterface::AddSoundModel(Stream *s,
                                    uint8_t *data,
                                    uint32_t data_size){

    int32_t status = 0;
    uint32_t num_models = 0;
    listen_model_type **in_models = nullptr;
    listen_model_type out_model = {};
    SoundModelInfo *sm_info = nullptr;

    PAL_VERBOSE(LOG_TAG, "Enter");
    if (GetSoundModelInfo(s)->GetModelData()) {
        PAL_DBG(LOG_TAG, "Stream model already added");
        return 0;
    }

    if (!sm_cfg_->isQCVAUUID() && !IsQCWakeUpConfigUsed()) {
        GetSoundModelInfo(s)->SetModelData(data, data_size);
        *sound_model_info_ = *GetSoundModelInfo(s);
        sm_merged_ = false;
        return 0;
    }

    /* Populate sound model info for the incoming stream model */
    status = QuerySoundModel(GetSoundModelInfo(s), data, data_size);
    if (status) {
        PAL_ERR(LOG_TAG, "QuerySoundModel failed status: %d", status);
        return status;
    }

    GetSoundModelInfo(s)->SetModelData(data, data_size);

    /* Check for remaining stream sound models to merge */
    for (auto &iter: sm_info_map_) {
        if (s != iter.first && iter.first && GetSoundModelInfo(iter.first)->GetModelData())
             num_models++;
    }

    if (!num_models) {
        PAL_DBG(LOG_TAG, "Copy model info from incoming stream to engine");
        *sound_model_info_ = *GetSoundModelInfo(s);
        sm_merged_ = false;
        return 0;
    }

    PAL_VERBOSE(LOG_TAG, "number of existing models: %d", num_models);
    /*
     * Merge this stream model with already existing merged model due to other
     * streams models.
     */
    if (!sound_model_info_) {
        PAL_ERR(LOG_TAG, "eng_sm_info is NULL");
        status = -EINVAL;
        goto cleanup;
    }

    if (!sound_model_info_->GetModelData()) {
        if (num_models == 1) {
            /*
             * Its not a merged model yet, but engine sm_data is valid
             * and must be pointing to single stream sm_data
             */
            PAL_ERR(LOG_TAG, "Model data is NULL, num_models: %d", num_models);
            status = -EINVAL;
            goto cleanup;
        } else if (!sm_merged_) {
            PAL_ERR(LOG_TAG, "Unexpected, no pre-existing merged model,"
                  "num current models %d", num_models);
            status = -EINVAL;
            goto cleanup;
        }
    }

    /* Merge this stream model with remaining streams models */
    num_models = 2;
    SoundModelInfo::AllocArrayPtrs((char***)&in_models, num_models,
                                   sizeof(listen_model_type));
    if (!in_models) {
        PAL_ERR(LOG_TAG, "in_models allocation failed");
        status = -ENOMEM;
        goto cleanup;
    }
    /* Add existing model */
    in_models[0]->data = sound_model_info_->GetModelData();
    in_models[0]->size = sound_model_info_->GetModelSize();
    /* Add incoming stream model */
    in_models[1]->data = data;
    in_models[1]->size = data_size;

    status = MergeSoundModels(num_models, in_models, &out_model);
    if (status) {
        PAL_ERR(LOG_TAG, "merge models failed");
        goto cleanup;
    }
    sm_info = new SoundModelInfo();
    sm_info->SetModelData(out_model.data, out_model.size);

    /* Populate sound model info for the merged stream models */
    status = QuerySoundModel(sm_info, out_model.data, out_model.size);
    if (status) {
        goto cleanup;
    }

    if (out_model.size < sound_model_info_->GetModelSize()) {
        PAL_ERR(LOG_TAG, "Unexpected, merged model sz %d < current sz %d",
            out_model.size, sound_model_info_->GetModelSize());
        status = -EINVAL;
        goto cleanup;
    }
    SoundModelInfo::FreeArrayPtrs((char **)in_models, num_models);
    in_models = nullptr;

    /* Update the new merged model */
    PAL_INFO(LOG_TAG, "Updated sound model: current size %d, new size %d",
        sound_model_info_->GetModelSize(), out_model.size);
    *sound_model_info_ = *sm_info;
    sm_merged_ = true;

    /*
     * Sound model merge would have changed the order of merge conf levels,
     * which need to be re-updated for all current active streams, if any.
     */
    status = UpdateMergeConfLevelsWithActiveStreams();
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to update merge conf levels, status = %d",
                                                                  status);
        goto cleanup;
    }

    delete sm_info;
    PAL_DBG(LOG_TAG, "Exit: status %d", status);
    return 0;

cleanup:
    if (out_model.data)
        free(out_model.data);

    if (in_models)
        SoundModelInfo::FreeArrayPtrs((char **)in_models, num_models);

    if (sm_info)
        delete sm_info;

    return status;
}

int32_t SVAInterface::DeleteFromMergedModel(
    char **keyphrases,
    uint32_t num_keyphrases,
    listen_model_type *in_model,
    listen_model_type *out_model) {

    listen_model_type merge_model = {};
    listen_status_enum sm_ret = kSucess;
    uint32_t out_model_sz = 0;
    int32_t status = 0;
    std::shared_ptr<SoundModelLib>sml = SoundModelLib::GetInstance();

    out_model->data = nullptr;
    out_model->size = 0;
    merge_model.data = in_model->data;
    merge_model.size = in_model->size;

    for (uint32_t i = 0; i < num_keyphrases; i++) {
        sm_ret = sml->GetSizeAfterDeleting_(&merge_model, keyphrases[i],
                                                   nullptr, &out_model_sz);
        if (sm_ret != kSucess) {
            PAL_ERR(LOG_TAG, "GetSizeAfterDeleting failed %d", sm_ret);
            status = -EINVAL;
            goto cleanup;
        }
        if (out_model_sz >= in_model->size) {
            PAL_ERR(LOG_TAG, "unexpected, GetSizeAfterDeleting returned size %d"
                  "not less than merged model size %d",
                  out_model_sz, in_model->size);
            status = -EINVAL;
            goto cleanup;
        }
        PAL_VERBOSE(LOG_TAG, "Size after deleting kw[%d] = %d", i, out_model_sz);
        if (!out_model->data) {
            /* Valid if deleting multiple keyphrases one after other */
            out_model->size = 0;
        }
        out_model->data = (uint8_t *)calloc(1, out_model_sz * sizeof(char));
        if (!out_model->data) {
            PAL_ERR(LOG_TAG, "Merge sound model allocation failed, size %d ",
                  out_model_sz);
            status = -ENOMEM;
            goto cleanup;
        }
        out_model->size = out_model_sz;

        sm_ret = sml->DeleteFromModel_(&merge_model, keyphrases[i],
                                              nullptr, out_model);
        if (sm_ret != kSucess) {
            PAL_ERR(LOG_TAG, "DeleteFromModel failed %d", sm_ret);
            status = -EINVAL;
            goto cleanup;
        }
        if (out_model->size != out_model_sz) {
            PAL_ERR(LOG_TAG, "unexpected, out_model size %d != expected size %d",
                  out_model->size, out_model_sz);
            status = -EINVAL;
            goto cleanup;
        }
        /* Used if deleting multiple keyphrases one after other */
        merge_model.data = out_model->data;
        merge_model.size = out_model->size;
    }

    if (VoiceUIPlatformInfo::GetInstance()->GetEnableDebugDumps()) {
        ST_DBG_DECLARE(FILE *sm_fd = NULL; static int sm_cnt = 0);
        ST_DBG_FILE_OPEN_WR(sm_fd, ST_DEBUG_DUMP_LOCATION,
            "st_smlib_output_deleted_sm", "bin", sm_cnt);
        ST_DBG_FILE_WRITE(sm_fd, merge_model.data, merge_model.size);
        ST_DBG_FILE_CLOSE(sm_fd);
        PAL_DBG(LOG_TAG, "SM returned from SML delete stored in: st_smlib_output_deleted_sm_%d.bin",
            sm_cnt);
        sm_cnt++;
    }
    return 0;

cleanup:
    if (out_model->data) {
        free(out_model->data);
        out_model->data = nullptr;
    }
    return status;
}

int32_t SVAInterface::DeleteSoundModel(
    Stream *s,
    struct detection_engine_config_voice_wakeup *wakeup_config) {

    int32_t status = 0;
    uint32_t num_models = 0;
    Stream *rem_st = nullptr;
    listen_model_type in_model = {};
    listen_model_type out_model = {};
    SoundModelInfo *sm_info = nullptr;

    PAL_VERBOSE(LOG_TAG, "Enter");
    if (!GetSoundModelInfo(s)->GetModelData()) {
        PAL_INFO(LOG_TAG, "Stream model data already deleted");
        return 0;
    }

    PAL_VERBOSE(LOG_TAG, "sm_data %pK, sm_size %d",
          GetSoundModelInfo(s)->GetModelData(),
          GetSoundModelInfo(s)->GetModelSize());

    /* Check for remaining streams sound models to merge */
    for (auto &iter: sm_info_map_) {
        if (s != iter.first && iter.first) {
             if (GetSoundModelInfo(iter.first) &&
                 GetSoundModelInfo(iter.first)->GetModelData()) {
                 rem_st = iter.first;
                 num_models++;
                 PAL_DBG(LOG_TAG, "num_models: %d", num_models);
             }
        }
    }

    if (num_models == 0) {
        PAL_DBG(LOG_TAG, "No remaining models");
        return 0;
    }
    if (num_models == 1) {
        PAL_DBG(LOG_TAG, "reuse only remaining stream model, size %d",
            GetSoundModelInfo(rem_st)->GetModelSize());
        /* If only one remaining stream model exists, re-use it */
        *sound_model_info_ = *GetSoundModelInfo(rem_st);
        wakeup_config->num_active_models = sound_model_info_->GetConfLevelsSize();
        for (int i = 0; i < sound_model_info_->GetConfLevelsSize(); i++) {
            if (sound_model_info_->GetConfLevels()) {
                wakeup_config->confidence_levels[i] = sound_model_info_->GetConfLevels()[i];
                wakeup_config->keyword_user_enables[i] =
                    (wakeup_config->confidence_levels[i] == 100) ? 0 : 1;
                PAL_DBG(LOG_TAG, "cf levels[%d] = %d", i, wakeup_config->confidence_levels[i]);
            }
        }
        sm_merged_ = false;
        return 0;
    }

    /*
     * Delete this stream model with already existing merged model due to other
     * streams models.
     */
    if (!sm_merged_ || !(sound_model_info_->GetModelData())) {
        PAL_ERR(LOG_TAG, "Unexpected, no pre-existing merged model to delete from,"
              "num current models %d", num_models);
        goto cleanup;
    }

    /* Existing merged model from which the current stream model to be deleted */
    in_model.data = sound_model_info_->GetModelData();
    in_model.size = sound_model_info_->GetModelSize();

    status = DeleteFromMergedModel(GetSoundModelInfo(s)->GetKeyPhrases(),
        GetSoundModelInfo(s)->GetNumKeyPhrases(), &in_model, &out_model);

    if (status)
        goto cleanup;
    sm_info = new SoundModelInfo();
    sm_info->SetModelData(out_model.data, out_model.size);

    /* Update existing merged model info with new merged model */
    status = QuerySoundModel(sm_info, out_model.data,
                               out_model.size);
    if (status)
        goto cleanup;

    if (out_model.size > sound_model_info_->GetModelSize()) {
        PAL_ERR(LOG_TAG, "Unexpected, merged model sz %d > current sz %d",
            out_model.size, sound_model_info_->GetModelSize());
        status = -EINVAL;
        goto cleanup;
    }

    PAL_INFO(LOG_TAG, "Updated sound model: current size %d, new size %d",
        sound_model_info_->GetModelSize(), out_model.size);

    *sound_model_info_ = *sm_info;
    sm_merged_ = true;

    /*
     * Sound model merge would have changed the order of merge conf levels,
     * which need to be re-updated for all current active streams, if any.
     */
    status = UpdateMergeConfLevelsWithActiveStreams();
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to update merge conf levels, status = %d",
                                                                  status);
        goto cleanup;
    }

    delete sm_info;
    PAL_DBG(LOG_TAG, "Exit: status %d", status);
    return 0;

cleanup:
    if (out_model.data)
        free(out_model.data);

    if (sm_info)
        delete sm_info;

    return status;
}

int32_t SVAInterface::UpdateEngineModel(
    Stream *s,
    uint8_t *data,
    uint32_t data_size,
    struct detection_engine_config_voice_wakeup *wakeup_config,
    bool add) {

    int32_t status = 0;

    if (add)
        status = AddSoundModel(s, data, data_size);
    else
        status = DeleteSoundModel(s, wakeup_config);

    PAL_DBG(LOG_TAG, "Exit, status: %d", status);
    return status;
}

int32_t SVAInterface::UpdateMergeConfLevelsPayload(
    SoundModelInfo *src_sm_info,
    bool set) {

    if (!src_sm_info) {
        PAL_ERR(LOG_TAG, "src sm info NULL");
        return -EINVAL;
    }

    if (!sm_merged_) {
        PAL_DBG(LOG_TAG, "Soundmodel is not merged, use source sm info");
        *sound_model_info_ = *src_sm_info;
        for (uint32_t i = 0; i < sound_model_info_->GetConfLevelsSize(); i++) {
            if (!set) {
                sound_model_info_->UpdateConfLevel(i, MAX_CONF_LEVEL_VALUE);
                PAL_INFO(LOG_TAG, "reset: cf_levels[%d]=%d",
                    i, sound_model_info_->GetConfLevels()[i]);
            }
        }
        return 0;
    }

    if (src_sm_info->GetConfLevelsSize() > sound_model_info_->GetConfLevelsSize()) {
        PAL_ERR(LOG_TAG, "Unexpected, stream conf levels sz > eng conf levels sz");
        return -EINVAL;
    }

    for (uint32_t i = 0; i < src_sm_info->GetConfLevelsSize(); i++)
        PAL_VERBOSE(LOG_TAG, "source cf levels[%d] = %d for %s", i,
            src_sm_info->GetConfLevels()[i], src_sm_info->GetConfLevelsKwUsers()[i]);

    /* Populate DSP merged sound model conf levels */
    for (uint32_t i = 0; i < src_sm_info->GetConfLevelsSize(); i++) {
        for (uint32_t j = 0; j < sound_model_info_->GetConfLevelsSize(); j++) {
            if (!strcmp(sound_model_info_->GetConfLevelsKwUsers()[j],
                        src_sm_info->GetConfLevelsKwUsers()[i])) {
                if (set) {
                    sound_model_info_->UpdateConfLevel(j, src_sm_info->GetConfLevels()[i]);
                    PAL_DBG(LOG_TAG, "set: cf_levels[%d]=%d",
                          j, sound_model_info_->GetConfLevels()[j]);
                } else {
                    sound_model_info_->UpdateConfLevel(j, MAX_CONF_LEVEL_VALUE);
                    PAL_DBG(LOG_TAG, "reset: cf_levels[%d]=%d",
                          j, sound_model_info_->GetConfLevels()[j]);
                }
            }
        }
    }

    for (uint32_t i = 0; i < sound_model_info_->GetConfLevelsSize(); i++)
        PAL_INFO(LOG_TAG, "engine cf_levels[%d] = %d",
            i, sound_model_info_->GetConfLevels()[i]);

    return 0;
}

int32_t SVAInterface::UpdateMergeConfLevelsWithActiveStreams() {

    int32_t status = 0;
    for (auto &iter: sm_info_map_) {
        if (iter.second->state == true) {
            PAL_VERBOSE(LOG_TAG, "update merge conf levels with other active streams");
            status = UpdateMergeConfLevelsPayload(GetSoundModelInfo(iter.first),
                        true);
            if (status)
                return status;
        }
    }
    return status;
}
