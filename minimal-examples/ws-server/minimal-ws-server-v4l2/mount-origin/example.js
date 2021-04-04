
function get_appropriate_ws_url(extra_url)
{
	var pcol;
	var u = document.URL;

	/*
	 * We open the websocket encrypted if this page came on an
	 * https:// url itself, otherwise unencrypted
	 */

	if (u.substring(0, 5) === "https") {
		pcol = "wss://";
		u = u.substr(8);
	} else {
		pcol = "ws://";
		if (u.substring(0, 4) === "http")
			u = u.substr(7);
	}

	u = u.split("/");

	/* + "/xxx" bit is for IE10 workaround */

	return pcol + u[0] + "/" + extra_url;
}

function new_ws(urlpath, protocol)
{
	return new WebSocket(urlpath, protocol);
}


var uw = window.URL || window.webkitURL;

document.addEventListener("DOMContentLoaded", function() {

	var ws = new_ws(get_appropriate_ws_url(""), "lws-v4l2");

	console.log(get_appropriate_ws_url(""));

	ws.binaryType = 'arraybuffer';
	try {
		ws.onopen = function() {

		};

		ws.onmessage = function got_packet(msg) {
		    var image = document.getElementById('img');
		    var bl  = new Blob([msg.data], { type: "image/jpeg" });

		    image.src = uw.createObjectURL(bl);
		    uw.revokeObjectURL(bl);
		};

		ws.onclose = function(){
		};
	} catch(exception) {
		alert("<p>Error " + exception);
	}

	function sendmsg()
	{
		ws.send(document.getElementById("m").value);
		document.getElementById("m").value = "";
	}

	//document.getElementById("b").addEventListener("click", sendmsg);

}, false);