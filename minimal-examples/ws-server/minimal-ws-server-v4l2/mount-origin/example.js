/*
 * based on
 * https://github.com/elsampsa/websocket-mse-demo/blob/master/ws_client_new.html
 */
var uw = window.URL || window.webkitURL;
var verbose = false;
var buffering_sec = 0; /* we want to keep this much in reserve in the local buffer */
    
var stream_started = false;
var ms;
var queue = [];
var stream_live;
var seeked = false;
var cc = 0;
var oneshot = 0;
var erste = [] , erste_ready = 0;
    
var source_buffer, codecpars;
var pass = 0;

var buffering_sec_seek = buffering_sec * 0.9; 
var buffering_sec_seek_distance = buffering_sec * 0.5; 

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

var ws;

function new_ws(urlpath, protocol)
{
	return new WebSocket(urlpath, protocol);
}


/*
 * taken from here: https://stackoverflow.com/questions/54186634/
 * sending-periodic-metadata-in-fragmented-live-mp4-stream/
 */
            
function toInt(arr, index)
{
	var dv = new DataView(arr.buffer, 0);

	return dv.getInt32(index, false);
}

function toString(arr, fr, to)
{
	return String.fromCharCode.apply(null, arr.slice(fr,to));
}

function getBox(arr, i)
{
	return [toInt(arr, i), toString(arr, i+4, i+8)]
}

function getSubBox(arr, box_name, _ofs, ifs)
{
        var i = _ofs;
        res = getBox(arr, i);

        main_length = res[0];
        name = res[1]; // this boxes length and name
        
       // console.log("subb: " + name + " len " + main_length);

        i = i + 8 + ifs;
        
        var sub_box = null;
        
        while (i < main_length + _ofs) {
            res = getBox(arr, i);
            l = res[0];
            name = res[1];
            
             // console.log("  inner: " + name + " l " + main_length);
            
            if (box_name == name) {
                sub_box = arr.slice(i, i+l);
                        return sub_box;
            }
            i = i + l;
        }
        return sub_box;
}

function hasFirstSampleFlag(arr, _ofs)
{       
        var traf = getSubBox(arr, "traf", _ofs, 0);
        if (traf==null) { console.log("no traf in moof"); return false; }
        
        var trun = getSubBox(traf, "trun", 0, 0);
        if (trun==null) { console.log("no trun in traf"); return false; }
        
        // ISO/IEC 14496-12:2012(E) .. pages 5 and 57
        // bytes: (size 4), (name 4), (version 1 + tr_flags 3)
        var flags = trun.slice(9,12);
        
//        console.log(flags);
        f = flags[2] & 4; // console.log(f);
        return f == 4;
    }
    
function hexdump(buffer, blockSize)
{	
	if (typeof buffer === 'string') {
	} else if (buffer instanceof ArrayBuffer && buffer.byteLength !== undefined) {
		buffer = String.fromCharCode.apply(String,
				[].slice.call(new Uint8Array(buffer)));
	} else if (Array.isArray(buffer)){
		buffer = String.fromCharCode.apply(String, buffer);
	} else if (buffer.constructor === Uint8Array) {
		buffer = String.fromCharCode.apply(String,
				[].slice.call(buffer));
	} else {
		console.log("Error: buffer is unknown...");
		return false;
	}	
    
	blockSize = blockSize || 16;
    var lines = [];
    var hex = "0123456789ABCDEF";
    for (var b = 0; b < buffer.length; b += blockSize) {
        var block = buffer.slice(b, Math.min(b + blockSize, buffer.length));
        var addr = ("0000" + b.toString(16)).slice(-4);
        var codes = block.split('').map(function (ch) {
            var code = ch.charCodeAt(0);
            return " " + hex[(0xF0 & code) >> 4] + hex[0x0F & code];
        }).join("");
        codes += "   ".repeat(blockSize - block.length);
        var chars = block.replace(/[\x00-\x1F\x20]/g, '.');
        chars +=  " ".repeat(blockSize - block.length);
        lines.push(addr + " " + codes + "  " + chars);
    }
    return lines.join("\n");
}

function dtox(d, padding)
{
	var hex = Number(d).toString(16);
	
	padding = typeof(padding) === "undefined" ||
			padding === null ? padding = 2 : padding;

	while (hex.length < padding)
        	hex = "0" + hex;

	return hex;
}

function ms_opened()
{
	if (oneshot)
		return;

        // https://developer.mozilla.org/en-US/docs/Web/API/MediaSource/duration
        console.log("mediasource.opened()");
        stream_live = document.getElementById('stream_live');
        URL.revokeObjectURL(stream_live.src);
        
        oneshot = 1;
        ms.duration = buffering_sec;

        source_buffer = ms.addSourceBuffer(codecPars);
        source_buffer.mode = 'sequence';
	
	console.log(ms.readyState);
        
        source_buffer.addEventListener("updateend", loadPacket);
        if (queue.length) {
        	console.log("ms_opened: loading queued pkt");
                loadPacket();
        }
}
    
function ms_closed() {
	console.log("mediasource closed()");
	ws.close();
}
    
function ms_ended() {
	console.log("mediasource ended()");
	ws.close();
}

var _appendBuffer = function(buffer1, buffer2) {
  var tmp = new Uint8Array(buffer1.byteLength + buffer2.byteLength);
  tmp.set(new Uint8Array(buffer1), 0);
  tmp.set(new Uint8Array(buffer2), buffer1.byteLength);
  return tmp.buffer;
};

var ofs = 0;
               
function putPacket(arr)
{     
	var memview   = new Uint8Array(arr);
	var sanity = 50;
        
	// console.log("putPacket: " + memview.byteLength);
 //       console.log(hexdump(arr, 16));

	ofs = 0;
	if (pass < 3) {
	
//	        console.log(hexdump(arr, 16));

		while (sanity-- && pass < 3 && ofs < memview.byteLength) {
	
		        res = getBox(memview, ofs);
		        main_length = res[0];
		        name = res[1];
		        
		        //console.log("box " + name + ", len " + main_length);

			switch (pass) {
			case 0:		        
		        	if (name != "styp" && name != "ftyp")
		        		break;
				pass++;
				break;
		        case 1:
		        	if (name !="moov")
		        		break;
		        	pass++;
				sb = getSubBox(memview, "trak", ofs, 0);
				if (!sb)
					break;
				sb = getSubBox(sb, "mdia", 0, 0);
				if (!sb)
					break;
				sb = getSubBox(sb, "minf", 0, 0);
				if (!sb)
					break;
				sb = getSubBox(sb, "stbl", 0, 0);
				if (!sb)
					break;
				sb = getSubBox(sb, "stsd", 0, 0);
				if (!sb)
					break;
				sb = getSubBox(sb, "avc1", 0, 8);
				if (!sb)
					break;
				sb = getSubBox(sb, "avcC", 0, 78);
				if (!sb)
					break;

				/* audio is tbd */
				
				codecPars = "video/mp4"+';codecs="avc1.' +
						dtox(sb[9]) + dtox(sb[10]) +
						dtox(sb[11]) +'"';

				if (!MediaSource.isTypeSupported(codecPars)) {
		                	console.log("Mimetype " + codecPars +
		                			" not supported");
		                	ws.close();
	                	} else
	                		console.log("Mimetype " + codecPars +
	                				" supported");

		        	break;
			case 2:
		        	if (name != "moof")
		        		break;
		            	if (hasFirstSampleFlag(memview, ofs)) {
		                	pass++;
					console.log("moof flags OK");
				} else
		                	return;
		                break;
			default:
				break;
		        }
	        
	                ofs += main_length;
		}
	}

	if (0 && pass < 3) {
		console.log("erste: adding " + arr.byteLength);
		if (!erste_ready)
			erste = arr;
		else
	        	erste = _appendBuffer(erste, arr);
        	erste_ready = 1;
        	console.log("done");
        	return;
        } 
        
        if (!ms) {
		ms = new MediaSource();
                ms.addEventListener('sourceopen', ms_opened, false);
                ms.addEventListener('sourceclosed', ms_closed, false);
                ms.addEventListener('sourceended', ms_ended, false);
                
                console.log("opening mediasource");
        	stream_live = document.getElementById('stream_live');
        	stream_live.src = window.URL.createObjectURL(ms);
	}
    
 	ofs = 0;
 	
 	if (erste_ready) {
 		console.log("draining erste");
 		arr = _appendBuffer(erste, arr);
 		// console.log(hexdump(arr, 16));
 		erste_ready = 0;
 	}
 
        // keep the latency to minimum
 
 	if (stream_live) {
	        let latest = stream_live.duration;

	        if ((stream_live.duration >= buffering_sec) && 
	            ((latest - stream_live.currentTime) > buffering_sec_seek)) {
	            //console.log("seek from ", stream_live.currentTime, " to ", latest);
	            df = (stream_live.duration - stream_live.currentTime); // this much away from the last available frame
	            if ((df > buffering_sec_seek)) {
	                seek_to = stream_live.duration - buffering_sec_seek_distance;
	                stream_live.currentTime = seek_to;
	                }
	        }
        }

        data = arr;
        if (queue.length == 0 && source_buffer && !source_buffer.updating) {

		try {

			console.log("direct, len " + data.byteLength);
			console.log(hexdump(data, 16));

			source_buffer.timestampOffset = ms.duration;
		    	source_buffer.appendBuffer(data);

		} catch(exc) {
			console.log("exception: source_buffer.appendBuffer " + exc);
			ws.close();
		 	return;
		};
    
       //     console.log(hexdump(arr, 16));

        //	cc = cc + 1;
        	return;
        }
        
        queue.push(data); // add to the end
        console.log("queue push:" + queue.length + ", len: " + data.byteLength);
    }
            
            
            
function loadPacket()
{	
	if (!queue.length)
		return;
	
	if (!source_buffer)
		return;

	stream_started = true;
                    
	inp = queue.shift();
		console.log("loadPacket " + ms.readyState + ", dur " + stream_live.duration + ", ms dur " + ms.duration + ", len " + inp.byteLength);
	
	if (verbose) { console.log("queue pop:", queue.length); }
                    
	var memview = new Uint8Array(inp);
	
	if (verbose) { console.log(" ==> writing buffer with", memview[0], memview[1], memview[2], memview[3]); }
	
	try {

//		console.log(hexdump(inp, 16));
//		source_buffer.timestampOffset = 0; //ms.duration;
		source_buffer.appendBuffer(inp);

	} catch(e) {
		console.log("appending buf failed");
	            	ws.close();
	            	return;
	}

	cc = cc + 1;
}

document.addEventListener("DOMContentLoaded", function() {
                
        ws = new_ws(get_appropriate_ws_url(""), "lws-v4l2");

	ws.binaryType = 'arraybuffer';
	try {
		ws.onopen = function() {
			ofs = 0;
		};

		ws.onmessage = function got_packet(msg) {
			// console.log("ws.onmessage");
		     putPacket(msg.data);
		};

		ws.onclose = function(){
			console.log("ws closed");
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
