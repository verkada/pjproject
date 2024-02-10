import inc_sdp as sdp
import inc_sip as sip

# Offer contains "inactive" attribute in the session, however the media
# also has "sendonly" attribute. Answer should appropriately respond
# direction attribute in media, instead of the one in session.
sdp = """
v=0
o=- 0 0 IN IP4 127.0.0.1
s=-
c=IN IP4 127.0.0.1
t=0 0
a=inactive
m=audio 5000 RTP/AVP 0
a=sendonly
"""

pjsua_args = "--null-audio --auto-answer 200"
extra_headers = ""
include = ["Content-Type: application/sdp", "a=recvonly"]  # response must include SDP
exclude = []

sendto_cfg = sip.SendtoCfg(
    "SDP direction in session",
    pjsua_args,
    sdp,
    200,
    extra_headers=extra_headers,
    resp_inc=include,
    resp_exc=exclude,
)
