/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Video for Jetpack.
 *
 * The Initial Developer of the Original Code is Mozilla Labs.
 * Portions created by the Initial Developer are Copyright (C) 2009-10
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Anant Narayanan <anant@kix.in>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "MediaRecorder.h"

NS_IMPL_ISUPPORTS1(MediaRecorder, IMediaRecorder)

MediaRecorder *MediaRecorder::gMediaRecordingService = nsnull;
MediaRecorder *
MediaRecorder::GetSingleton()
{
    if (gMediaRecordingService) {
        gMediaRecordingService->AddRef();
        return gMediaRecordingService;
    }
    gMediaRecordingService = new MediaRecorder();
    if (gMediaRecordingService) {
        gMediaRecordingService->AddRef();
        if (NS_FAILED(gMediaRecordingService->Init()))
            gMediaRecordingService->Release();
    }
    return gMediaRecordingService;
}


/*
 * === Here's the meat of the code. Static callbacks & encoder ===
 */

void
MediaRecorder::WriteAudio(void *data)
{
    int ret;
    nsresult rv;
    PRUint32 wr;
    MediaRecorder *mr = static_cast<MediaRecorder*>(data);

    while (vorbis_analysis_blockout(&mr->aState->vd, &mr->aState->vb) == 1) {
        vorbis_analysis(&mr->aState->vb, NULL);
        vorbis_bitrate_addblock(&mr->aState->vb);
        while (vorbis_bitrate_flushpacket(
                &mr->aState->vd, &mr->aState->op)) {
            ogg_stream_packetin(&mr->aState->os, &mr->aState->op);

            for (;;) {
                ret = ogg_stream_pageout(&mr->aState->os, &mr->aState->og);
                if (ret == 0)
                    break;

                rv = mr->WriteData(
                    mr, mr->aState->og.header, mr->aState->og.header_len, &wr
                );
                rv = mr->WriteData(
                    mr, mr->aState->og.body, mr->aState->og.body_len, &wr
                );

                if (ogg_page_eos(&mr->aState->og))
                    break;
            }
        }
    }
}

nsresult
MediaRecorder::WriteData(
    void *obj, unsigned char *data, PRUint32 len, PRUint32 *wr)
{
    MediaRecorder *mr = static_cast<MediaRecorder*>(obj);
    return mr->pipeStream->Write((const char*)data, len, wr);
}


/*
 * Encoder. Runs in a loop off the record thread until we run out of frames
 * (probably because stop was called)
 */
void
MediaRecorder::Encode(void *data)
{
    int i, j;
    nsresult rv;
    PRUint32 wr;
    PRUint32 rd = 0;

    PRInt16 *a_frame;
    PRUint8 *v_frame;
    float **a_buffer;
    th_ycbcr_buffer v_buffer;
    MediaRecorder *mr = static_cast<MediaRecorder*>(data);

    /* Alright, so we are multiplexing audio with video. We first fetch 1 frame
     * of video from the pipe, encode it and then follow up with packets of
     * audio. We want the audio and video packets to be as close to each other
     * as possible (timewise) to make playback synchronized.
     *
     * The generic formula to decide how many bytes of audio we must write per
     * frame of video will be: (SAMPLE_RATE / FPS) * AUDIO_FRAME_SIZE;
     *
     * This works out to 8820 bytes of 22050Hz audio (2205 frames)
     * per frame of video @ 10 fps (these are the default values).
     *
     * Sure, this assumes that the audio and video recordings start at exactly
     * the same time. While that may not be true if you look at the
     * millisecond level, our brain does an amazing job of approximating and
     * WANTS to see synchronized audio and video. It all works out just fine :)
     *
     * OR DOES IT? FIXME!
     *
     * TODO: Figure out if PR_Calloc will be more efficient if we call it for
     * storing more than just 1 frame at a time. For instance, we can wait
     * a second and encode 10 frames of video and 88200 bytes of audio per
     * run of the loop?
     */
    int a_frame_len = FRAMES_BUFFER;
    if (mr->v_rec) {
        a_frame_len = mr->params->rate/(mr->params->fps_n/mr->params->fps_d);
    }
    int v_frame_size = mr->vState->backend->GetFrameSize();
    int a_frame_size = mr->aState->backend->GetFrameSize();
    int a_frame_total = a_frame_len * a_frame_size;
        
    for (;;) {

        if (mr->v_rec) {

        /* Make sure we get 1 full frame of video */
        v_frame = (PRUint8 *)PR_Calloc(v_frame_size, sizeof(PRUint8));
        do mr->vState->vPipeIn->Available(&rd);
            while ((rd < (PRUint32)v_frame_size) && !mr->v_stp);
        rv = mr->vState->vPipeIn->Read((char *)v_frame, v_frame_size, &rd);

        if (rd == 0) {
            /* EOF. If there's audio we need to finish it. Goto: gasp! */
            if (mr->a_rec) {
                PR_Free(v_frame);
                goto audio_enc;
            } else {
                PR_Free(v_frame);
                return;
            }
        } else if (rd != (PRUint32)v_frame_size) {
            /* Now, we're in a pickle. HOW TO FIXME? */
            PR_LOG(mr->log, PR_LOG_NOTICE,
                ("only read %u of %d from video pipe\n", rd, v_frame_size));
            PR_Free(v_frame);
            return;
        }

        /* Convert i420 to YCbCr */
        v_buffer[0].width = mr->params->width;
        v_buffer[0].stride = mr->params->width;
        v_buffer[0].height = mr->params->height;

        v_buffer[1].width = (v_buffer[0].width >> 1);
        v_buffer[1].height = (v_buffer[0].height >> 1);
        v_buffer[1].stride = v_buffer[1].width;

        v_buffer[2].width = v_buffer[1].width;
        v_buffer[2].height = v_buffer[1].height;
        v_buffer[2].stride = v_buffer[1].stride;

        v_buffer[0].data = v_frame;
        v_buffer[1].data = v_frame + v_buffer[0].width * v_buffer[0].height;
        v_buffer[2].data =
            v_buffer[1].data + v_buffer[0].width * v_buffer[0].height / 4;

        /* Encode 'er up */
        if (th_encode_ycbcr_in(mr->vState->th, v_buffer) != 0) {
            PR_LOG(mr->log, PR_LOG_NOTICE, ("Could not encode frame\n"));
            PR_Free(v_frame);
            return;
        }
        if (!th_encode_packetout(mr->vState->th, 0, &mr->vState->op)) {
            PR_LOG(mr->log, PR_LOG_NOTICE, ("Could not read packet\n"));
            PR_Free(v_frame);
            return;
        }

        ogg_stream_packetin(&mr->vState->os, &mr->vState->op);
        while (ogg_stream_pageout(&mr->vState->os, &mr->vState->og)) {
            rv = mr->WriteData(
                mr, mr->vState->og.header, mr->vState->og.header_len, &wr
            );
            rv = mr->WriteData(
                mr, mr->vState->og.body, mr->vState->og.body_len, &wr
            );
        }
        PR_Free(v_frame);

        }

audio_enc:

        if (mr->a_rec) {

        /* Make sure we get enough frames in unless we're at the end */
        a_frame = (PRInt16 *) PR_Calloc(a_frame_total, sizeof(PRUint8));

        do mr->aState->aPipeIn->Available(&rd);
            while ((rd < (PRUint32)a_frame_total) && !mr->a_stp);
        rv = mr->aState->aPipeIn->Read((char *)a_frame, a_frame_total, &rd);
        
        if (rd == 0) {
            /* EOF. We're done. */
            PR_Free(a_frame);
            return;
        } else if (rd != (PRUint32)a_frame_total) {
            /* Hmm. I sure hope this is the end of the recording. */
            PR_Free(a_frame);
            if (!mr->a_stp) {
                PR_LOG(mr->log, PR_LOG_NOTICE,
                    ("only read %u of %d from audio pipe\n", rd, a_frame_total));
            }
            return;
        }

        /* Uninterleave samples */
        a_buffer = vorbis_analysis_buffer(&mr->aState->vd, a_frame_len);
        for (i = 0; i < a_frame_len; i++){
            for (j = 0; j < (int)mr->params->chan; j++) {
                a_buffer[j][i] = (float)((float)a_frame[i+j] / 32768.f);
            }
        }

        /* Tell libvorbis to do its thing */
        vorbis_analysis_wrote(&mr->aState->vd, a_frame_len);
        MediaRecorder::WriteAudio(data);
        PR_Free(a_frame);

        }
        /* We keep looping until we reach EOF on either pipe */
    }
}


/*
 * === Class methods begin now! ===
 */
nsresult
MediaRecorder::Init()
{
    a_rec = PR_FALSE;
    v_rec = PR_FALSE;

    log = PR_NewLogModule("MediaRecorder");

    pipeStream = nsnull;
    params = (Properties *)PR_Calloc(1, sizeof(Properties));
    aState = (Audio *)PR_Calloc(1, sizeof(Audio));
    vState = (Video *)PR_Calloc(1, sizeof(Video));
    return NS_OK;
}

MediaRecorder::~MediaRecorder()
{
    PR_Free(params);
    PR_Free(vState);
    PR_Free(aState);

    gMediaRecordingService = nsnull;
}

/*
 * Theora beginning of stream
 */
nsresult
MediaRecorder::SetupTheoraBOS()
{
    int i;
    nsresult rv;
    PRUint32 wr;
    ogg_uint32_t keyframe;

    if (ogg_stream_init(&vState->os, rand())) {
        PR_LOG(log, PR_LOG_NOTICE, ("Failed ogg_stream_init\n"));
        return NS_ERROR_FAILURE;
    }

    th_info_init(&vState->ti);

    /* Must be multiples of 16 */
    vState->ti.frame_width = (params->width + 15) & ~0xF;
    vState->ti.frame_height = (params->height + 15) & ~0xF;
    vState->ti.pic_width = params->width;
    vState->ti.pic_height = params->height;
    vState->ti.pic_x = (vState->ti.frame_width - params->width) >> 1 & ~1;
    vState->ti.pic_y = (vState->ti.frame_height - params->height) >> 1 & ~1;
    vState->ti.fps_numerator = params->fps_n;
    vState->ti.fps_denominator = params->fps_d;

    /* Are these the right values? */
    keyframe = 64 - 1;
    for (i = 0; keyframe; i++)
        keyframe >>= 1;
    vState->ti.quality = (int)(params->qual * 100);
    vState->ti.colorspace = TH_CS_ITU_REC_470M;
    vState->ti.pixel_fmt = TH_PF_420;
    vState->ti.keyframe_granule_shift = i;

    vState->th = th_encode_alloc(&vState->ti);
    th_info_clear(&vState->ti);

    /* Header init */
    th_comment_init(&vState->tc);
    th_comment_add_tag(&vState->tc, (char *)"ENCODER", (char *)"rainbow");
    if (th_encode_flushheader(
        vState->th, &vState->tc, &vState->op) <= 0) {
        PR_LOG(log, PR_LOG_NOTICE, ("Internal Theora library error\n"));
        return NS_ERROR_FAILURE;
    }
    th_comment_clear(&vState->tc);

    ogg_stream_packetin(&vState->os, &vState->op);
    if (ogg_stream_pageout(&vState->os, &vState->og) != 1) {
        PR_LOG(log, PR_LOG_NOTICE, ("Internal Ogg library error\n"));
        return NS_ERROR_FAILURE;
    }

    rv = WriteData(
        this, vState->og.header, vState->og.header_len, &wr
    );
    rv = WriteData(
        this, vState->og.body, vState->og.body_len, &wr
    );

    return NS_OK;
}

/*
 * Theora stream headers
 */
nsresult
MediaRecorder::SetupTheoraHeaders()
{
    int ret;
    nsresult rv;
    PRUint32 wr;

    /* Create rest of the theora headers */
    for (;;) {
        ret = th_encode_flushheader(
            vState->th, &vState->tc, &vState->op
        );
        if (ret < 0){
            PR_LOG(log, PR_LOG_NOTICE, ("Internal Theora library error\n"));
            return NS_ERROR_FAILURE;
        } else if (!ret) break;
        ogg_stream_packetin(&vState->os, &vState->op);
    }
    /* Flush the rest of theora headers. */
    for (;;) {
        ret = ogg_stream_flush(&vState->os, &vState->og);
        if (ret < 0){
            PR_LOG(log, PR_LOG_NOTICE, ("Internal Ogg library error\n"));
            return NS_ERROR_FAILURE;
        }
        if (ret == 0) break;
        rv = WriteData(
            this, vState->og.header, vState->og.header_len, &wr
        );
        rv = WriteData(
            this, vState->og.body, vState->og.body_len, &wr
        );
    }

    return NS_OK;
}

/*
 * Vorbis beginning of stream
 */
nsresult
MediaRecorder::SetupVorbisBOS()
{
    int ret;
    nsresult rv;
    PRUint32 wr;

    if (ogg_stream_init(&aState->os, rand())) {
        PR_LOG(log, PR_LOG_NOTICE, ("Failed ogg_stream_init\n"));
        return NS_ERROR_FAILURE;
    }

    vorbis_info_init(&aState->vi);
    ret = vorbis_encode_init_vbr(
        &aState->vi, params->chan, params->rate, (float)params->qual
    );
    if (ret) {
        PR_LOG(log, PR_LOG_NOTICE, ("Failed vorbis_encode_init\n"));
        return NS_ERROR_FAILURE;
    }

    vorbis_comment_init(&aState->vc);
    vorbis_comment_add_tag(&aState->vc, "ENCODER", "rainbow");
    vorbis_analysis_init(&aState->vd, &aState->vi);
    vorbis_block_init(&aState->vd, &aState->vb);

    {
        ogg_packet header;
        ogg_packet header_comm;
        ogg_packet header_code;

        vorbis_analysis_headerout(
            &aState->vd, &aState->vc,
            &header, &header_comm, &header_code
        );
        ogg_stream_packetin(&aState->os, &header);
        ogg_stream_packetin(&aState->os, &header_comm);
        ogg_stream_packetin(&aState->os, &header_code);

        if (ogg_stream_pageout(&aState->os, &aState->og) != 1) {
            PR_LOG(log, PR_LOG_NOTICE, ("Internal Ogg library error\n"));
            return NS_ERROR_FAILURE;
        }

        rv = WriteData(
            this, aState->og.header, aState->og.header_len, &wr
        );
        rv = WriteData(
            this, aState->og.body, aState->og.body_len, &wr
        );
    }

    return NS_OK;
}

/*
 * Setup vorbis stream headers
 */
nsresult
MediaRecorder::SetupVorbisHeaders()
{
    int ret;
    nsresult rv;
    PRUint32 wr;

    while ((ret = ogg_stream_flush(&aState->os, &aState->og)) != 0) {
        rv = WriteData(
            this, aState->og.header, aState->og.header_len, &wr
        );
        rv = WriteData(
            this, aState->og.body, aState->og.body_len, &wr
        );
    }

    return NS_OK;
}

/*
 * Make a pipe (since NS_NewPipe2 isn't exposed by XPCOM)
 */
nsresult
MediaRecorder::MakePipe(nsIAsyncInputStream **in,
            nsIAsyncOutputStream **out)
{
    nsCOMPtr<nsIPipe> pipe = do_CreateInstance("@mozilla.org/pipe;1");
    if (!pipe)
        return NS_ERROR_OUT_OF_MEMORY;

    /* Video frame size > audio frame so we optimize for that */
    int size = params->width * params->height * 4;
    nsresult rv = pipe->Init(
        PR_FALSE, PR_FALSE,
        size, FRAMES_BUFFER, nsnull
    );
    if (NS_FAILED(rv))
        return rv;

    pipe->GetInputStream(in);
    pipe->GetOutputStream(out);
    return NS_OK;
}

/*
 * Parse and set properties
 */
void
MediaRecorder::ParseProperties(nsIPropertyBag2 *prop)
{
    nsresult rv;

    rv = prop->GetPropertyAsBool(NS_LITERAL_STRING("audio"), &params->audio);
    if (NS_FAILED(rv)) params->audio = PR_TRUE;
    rv = prop->GetPropertyAsBool(NS_LITERAL_STRING("video"), &params->video);
    if (NS_FAILED(rv)) params->video = PR_TRUE;
    rv = prop->GetPropertyAsUint32(NS_LITERAL_STRING("width"), &params->width);
    if (NS_FAILED(rv)) params->width = WIDTH;
    rv = prop->GetPropertyAsUint32(NS_LITERAL_STRING("height"), &params->height);
    if (NS_FAILED(rv)) params->height = HEIGHT;
    rv = prop->GetPropertyAsUint32(NS_LITERAL_STRING("channels"), &params->chan);
    if (NS_FAILED(rv)) params->chan = NUM_CHANNELS;
    rv = prop->GetPropertyAsUint32(NS_LITERAL_STRING("rate"), &params->rate);
    if (NS_FAILED(rv)) params->rate = SAMPLE_RATE;
    rv = prop->GetPropertyAsDouble(NS_LITERAL_STRING("quality"), &params->qual);
    if (NS_FAILED(rv)) params->qual = (double)SAMPLE_QUALITY;
    rv = prop->GetPropertyAsBool(NS_LITERAL_STRING("source"), &params->canvas);
    if (NS_FAILED(rv)) params->source = PR_FALSE;
}

/*
 * Start recording to file
 */
NS_IMETHODIMP
MediaRecorder::RecordToFile(
    nsIPropertyBag2 *prop,
    nsIDOMCanvasRenderingContext2D *ctx,
    nsILocalFile *file,
    nsIMediaStateObserver *obs
)
{
    nsresult rv;
    ParseProperties(prop);

    /* Get a file stream from the local file */
    canvas = ctx;
    observer = obs;
    nsCOMPtr<nsIFileOutputStream> stream(
        do_CreateInstance("@mozilla.org/network/file-output-stream;1")
    );

    pipeStream = do_QueryInterface(stream, &rv);
    if (NS_FAILED(rv)) return rv;
    rv = stream->Init(file, -1, -1, 0);
    if (NS_FAILED(rv)) return rv;

    thread = PR_CreateThread(
        PR_SYSTEM_THREAD,
        MediaRecorder::Record, this,
        PR_PRIORITY_NORMAL,
        PR_GLOBAL_THREAD,
        PR_JOINABLE_THREAD, 0
    );
    
    return NS_OK;
}

/*
 * Start recording (called in a thread)
 */
void
MediaRecorder::Record(void *data)
{   
    nsresult rv;
    MediaRecorder *mr = static_cast<MediaRecorder*>(data);
    Properties *params = mr->params;
    
    if (mr->a_rec || mr->v_rec) {
        mr->observer->OnStateChange("error", "recording already in progress");
        return;
    }

    /* Setup backends */
    #ifdef RAINBOW_Mac
    mr->aState->backend = new AudioSourceMac(params->chan, params->rate);
    mr->vState->backend = new VideoSourceMac(params->width, params->height);
    #endif
    #ifdef RAINBOW_Win
    mr->aState->backend = new AudioSourceWin(params->chan, params->rate);
    mr->vState->backend = new VideoSourceWin(params->width, params->height);
    #endif
    #ifdef RAINBOW_Nix
    mr->aState->backend = new AudioSourceNix(params->chan, params->rate);
    mr->vState->backend = new VideoSourceNix(params->width, params->height);
    #endif
 
    /* Is the given canvas source or destination? */
    if (params->canvas) {
        mr->vState->backend = new VideoSourceCanvas(params->width, params->height);
    }
       
    /* Update parameters. What we asked for were just hints,
     * may not end up the same */
    params->fps_n = mr->vState->backend->GetFPSN();
    params->fps_d = mr->vState->backend->GetFPSD();
    params->rate = mr->aState->backend->GetRate();
    params->chan = mr->aState->backend->GetChannels();

    /* FIXME: device detection TBD */
    if (params->audio && (mr->aState == nsnull)) {
        mr->observer->OnStateChange("error", "audio requested but no devices found");
        return;
    }
    if (params->video && (mr->vState == nsnull)) {
        mr->observer->OnStateChange("error", "video requested but no devices found");
        return;
    }

    /* Get ready for video! */
    if (params->video) {
        mr->SetupTheoraBOS();
        rv = mr->MakePipe(
            getter_AddRefs(mr->vState->vPipeIn),
            getter_AddRefs(mr->vState->vPipeOut)
        );
        if (NS_FAILED(rv)) {
            mr->observer->OnStateChange("error", "internal: could not create video pipe");
            return;
        }
    }

    /* Get ready for audio! */
    if (params->audio) {
        mr->SetupVorbisBOS();
        rv = mr->MakePipe(
            getter_AddRefs(mr->aState->aPipeIn),
            getter_AddRefs(mr->aState->aPipeOut)
        );
        if (NS_FAILED(rv)) {
            mr->observer->OnStateChange("error", "internal: could not create audio pipe");
            return;
        }
    }

    /* Let's DO this. */
    if (params->video) {
        mr->SetupTheoraHeaders();
        rv = mr->vState->backend->Start(mr->vState->vPipeOut, mr->canvas);
        if (NS_FAILED(rv)) {
            mr->observer->OnStateChange("error", "internal: could not start video recording");
            return;
        }
        mr->v_rec = PR_TRUE;
        mr->v_stp = PR_FALSE;
    }
    if (params->audio) {
        mr->SetupVorbisHeaders();
        rv = mr->aState->backend->Start(mr->aState->aPipeOut);
        if (NS_FAILED(rv)) {
            /* FIXME: Stop and clean up video! */
            mr->observer->OnStateChange("error", "internal: could not start video recording");
            return;
        }
        mr->a_rec = PR_TRUE;
        mr->a_stp = PR_FALSE;
    }

    /* Start off encoder after notifying observer */
    mr->observer->OnStateChange("started", "");
    mr->Encode(data);
    return;
}

/*
 * Stop recording
 */
NS_IMETHODIMP
MediaRecorder::Stop()
{
    if (!a_rec && !v_rec) {
        observer->OnStateChange("error", "no recording in progress");
        return NS_ERROR_FAILURE;
    }

    /* Return quickly and actually stop in a thread, notifying caller via
     * the 'observer' */
    PR_CreateThread(
        PR_SYSTEM_THREAD,
        MediaRecorder::StopRecord, this,
        PR_PRIORITY_NORMAL,
        PR_GLOBAL_THREAD,
        PR_JOINABLE_THREAD, 0
    );
    
    return NS_OK;
}

void
MediaRecorder::StopRecord(void *data)
{
    nsresult rv;
    PRUint32 wr;
    MediaRecorder *mr = static_cast<MediaRecorder*>(data);
    
    if (mr->v_rec) {
        rv = mr->vState->backend->Stop();
        if (NS_FAILED(rv)) {
            mr->observer->OnStateChange("error", "could not stop video recording");
            return;
        }
    }

    if (mr->a_rec) {
        rv = mr->aState->backend->Stop();
        if (NS_FAILED(rv)) {
            mr->observer->OnStateChange("error", "could not stop audio recording");
            return;
        }
    }

    /* Wait for encoder to finish */
    if (mr->v_rec) {
        mr->v_stp = PR_TRUE;
        mr->vState->vPipeOut->Close();
    }
    if (mr->a_rec) {
        mr->a_stp = PR_TRUE;
        mr->aState->aPipeOut->Close();
    }

    PR_JoinThread(mr->thread);

    if (mr->v_rec) {
        mr->vState->vPipeIn->Close();
        th_encode_free(mr->vState->th);

        /* Video trailer */
        if (ogg_stream_flush(&mr->vState->os, &mr->vState->og)) {
            rv = WriteData(
                mr, mr->vState->og.header, mr->vState->og.header_len, &wr
            );
            rv = WriteData(
                mr, mr->vState->og.body, mr->vState->og.body_len, &wr
            );
        }

        ogg_stream_clear(&mr->vState->os);
        mr->v_rec = PR_FALSE;
    }

    if (mr->a_rec) {
        mr->aState->aPipeIn->Close();

        /* Audio trailer */
        vorbis_analysis_wrote(&mr->aState->vd, 0);
        MediaRecorder::WriteAudio(mr);

        vorbis_block_clear(&mr->aState->vb);
        vorbis_dsp_clear(&mr->aState->vd);
        vorbis_comment_clear(&mr->aState->vc);
        vorbis_info_clear(&mr->aState->vi);
        ogg_stream_clear(&mr->aState->os);
        mr->a_rec = PR_FALSE;
    }

    /* GG */
    mr->pipeStream->Close();
    mr->observer->OnStateChange("stopped", "");
    return;
}

