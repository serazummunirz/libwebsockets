# lws minimal ws server v4l2

## build

```
 $ cmake . && make
```

## Commandline Options

Option|Meaning
---|---
-d|Set logging verbosity
-s|Serve using TLS selfsigned cert (ie, connect to it with https://...)
-h|Strict Host: header checking against vhost name (localhost) and port
-v|Video device, default `/dev/video0`

## usage

```
[2021/04/05 15:00:59:4781] U: LWS minimal ws server V4L2 | visit http://localhost:7681
[2021/04/05 15:00:59:4782] N: LWS: 4.1.99-v4.2-rc1-66-gb85730c8, loglevel 1031
[2021/04/05 15:00:59:4782] N: NET CLI SRV H1 H2 WS SS-JSON-POL ConMon IPv6-absent
[2021/04/05 15:00:59:4782] N:  ++ [wsi|0|pipe] (1)
[2021/04/05 15:00:59:4782] N:  ++ [vh|0|netlink] (1)
[2021/04/05 15:00:59:4783] N:  ++ [vh|1|localhost||7681] (2)
[2021/04/05 15:00:59:4783] N: lws_socket_bind: nowsi: source ads 0.0.0.0
[2021/04/05 15:00:59:4783] N:  ++ [wsi|1|listen|localhost||7681] (2)
[2021/04/05 15:00:59:4783] N:  ++ [wsisrv|0|adopted] (1)
[2021/04/05 15:00:59:4784] N: lws_role_call_adoption_bind: falling back to raw file role bind
[2021/04/05 15:00:59:4784] N: LWS_CALLBACK_RAW_ADOPT_FILE
[2021/04/05 15:00:59:4784] N: cap 0x84a00001, bus_info usb-0000:46:00.1-4.2, driver uvcvideo, card USB Video: USB Video
[2021/04/05 15:00:59:4784] N: 0 Camera 1 2
[2021/04/05 15:00:59:4801] N: 1 1280 720 1 1843200
Brightness
Contrast
Saturation
Hue
[2021/04/05 15:00:59:4811] N: callback_v4l2: adopt completed ok
[2021/04/05 15:00:59:4811] N: lws_role_call_adoption_bind: falling back to raw file role bind
[2021/04/05 15:00:59:4812] N: callback_v4l2: leaving PROTOCOL_INIT OK
[2021/04/05 15:01:02:0718] N:  ++ [wsisrv|1|adopted] (2)
[2021/04/05 15:01:02:1676] N:  ++ [wsisrv|2|adopted] (3)
[2021/04/05 15:01:02:1678] N: start_capturing: stream on

```

Visit http://localhost:7681 (or https with -s) on one or more browser windows

Frames from the video device are sent to all the connected browsers, as fast as
each can take them.
