
# Video Integration

Summary: Added a fake video source that just generates dummy frames. When it comes to Intercom Encoder, it just ignores input video frames, instead pulls from video frames from `Intercom Camera Subsystem` over TCP port `20002` and feeds into the framework. `PJSIP` takes care of rest of the stuff.

## List of changes

1. PJSIP core video framework changes
    * Intercom Encoder and FFMPEG both publishes H264 encoder capability. Couldn't figure out an easy to prioritize Intercom Encoder over FFMPEG. So, disabled FFMPEG.
    * `PJSIP` by default enables video for both `send` and `receive`. It will create `SDL Window` and do some configurations there which causes issues. So, forced video `SDP` params to `sendonly` mode. This is done in `pjsua_media.c` file with forcing `PJMEDIA_DIR_ENCODING` value.
    * Disabled sending `SEI` packets in PJSIP H264 packetizer, as SIP phones were continuously complaining about decoder errors.

2. New Plugins
    * `intercom_dev.c` : This is a dummy video source plugin. It just fills video frame size and other parameters appropriately and sends it to PJSIP. Not writing / reading anything on video frames.
    * `intercom_codec.c` : It reads data from `Intercom Camera Subsystem` over TCP port `20002` and feeds into PJSIP. This module has lot of ffmpeg stub code to publish capabilities and other things, This will be removed eventually.

3. Helper utilities copied from `Camera code`
    * `nalparse.c` does socket read, aggregating RAW data in `NAL` units. Code is copied from [camera code](https://github.com/verkada/camera-firmware/blob/next/verkada/camera/vcamera/rtspd/utils/nalparse.cpp) 
    * Camera stores the timestamp in custom `SEI` packet. `get_timestamp_from_sei` extract this timestamp, That code is copied from [RTSP H264 source](https://github.com/verkada/camera-firmware/blob/90db0a40aa52d752768facd61523457581154baa/verkada/camera/vcamera/rtspd/liveMedia/H264StreamSource.cpp#L81)


## FAQ

1. Does it support streaming to multiple client ?

    Yes. `intercom_codec.c` will create a new socket for each client and will send data to all clients.

2. What if I enable auto video transmission for all calls, would it send video to phones which doesn't support video ?

    No. When auto video transmission is enabled, it will just include video in SDP. If client doesn't include video in reply SDP, framework will not send video. So even if it's enabled, it won't send video data to Twilio / Zoom clients.

3. Why a fake codec and source is needed, why not only fake video source ?

   PJSIP framework is written in a way that assumes video sources capture only RAW formats. Those frames must be passed go through a codec. There is no easy way to bypass video codec. So, Even if a video source produces H264 frames, it will be passed through a H264 encoder. That's why a fake codec is needed.

4. Why port `20002` is hard coded in `intercom_codec.c` ? 

   Couldn't find a way to pass this information using existing `PJSUA2` APIs. So to avoid major changes, it's hard coded.

5. Why `test_encode --stream 4 --force-idr` is needed ?
  
   New stream must start with `IDR` frame. Otherwise, SIP phones will complain about decoder errors. As per [this slack thread](https://verkada.slack.com/archives/C018R8MAARG/p1692035898030479?thread_ts=1692032768.072699&cid=C018R8MAARG), It should be safe to force `IDR` frame on this stream.