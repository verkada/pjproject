import inc_sdp as sdp
import inc_sip as sip

sdp = """
v=0
o=- 0 0 IN IP4 127.0.0.1
s=pjmedia
c=IN IP4 127.0.0.1
t=0 0
m=video 4000 RTP/AVP 101
a=rtpmap:101 my-proprietary-codec
"""

pjsua_args = "--null-audio --auto-answer 200"
extra_headers = ""
include = []
exclude = []

sendto_cfg = sip.SendtoCfg(
    "Unsupported codec",
    pjsua_args,
    sdp,
    488,
    extra_headers=extra_headers,
    resp_inc=include,
    resp_exc=exclude,
)
