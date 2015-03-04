import struct
import socket

from ampsave.exceptions import AmpTestVersionMismatch

# TODO fix stupidly long lines


# version needs to keep up with the version number in src/tests/http/http.h
AMP_HTTP_TEST_VERSION = 2015030400
PREVIOUS_AMP_HTTP_TEST_VERSION = 2014091900

def get_data(data):
    """
    Extract the HTTP test results from the data blob.

    The test result data consists of a single http_report_header_t
    followed by a number of http_report_server_t structures each with
    http_report_object_t structures describing all the objects. All of
    these are described in src/tests/http/http.h
    """

    if len(data) < struct.calcsize("=I"):
        print "%s: not enough data to unpack version", __file__
        return None

    version, = struct.unpack_from("=I", data, 0)
    offset = struct.calcsize("=II")

    if version == AMP_HTTP_TEST_VERSION:
        header_len = struct.calcsize("=II512sIIHHHHHBB2sBB")
    elif version == PREVIOUS_AMP_HTTP_TEST_VERSION:
        header_len = struct.calcsize("=II512sIIHBBBBBB6sBB")
    else:
        raise AmpTestVersionMismatch(version, AMP_HTTP_TEST_VERSION)

    server_len = struct.calcsize("=256sQQQQ46sHiHBB")
    object_len = struct.calcsize("=256sQQQQQQQQQQQQII6sBB")
    cache_len = struct.calcsize("=ii5sbbB")

    # check the version number first before looking at anything else
    if len(data) < header_len:
        print "%s: not enough data to unpack header", __file__
        return None

    # read the rest of the header that records test options
    if version == AMP_HTTP_TEST_VERSION:
        url,dur,size,obj,servers,max_con,max_con_ps,max_ps_ps,persist,pipe,pad,pipe_max,cache = struct.unpack_from("=512sIIHHHHHBB2sBB", data, offset)
    elif version == PREVIOUS_AMP_HTTP_TEST_VERSION:
        url,dur,size,obj,servers,persist,max_con,max_con_ps,max_ps_ps,pipe,pad,pipe_max,cache = struct.unpack_from("=512sIIHBBBBBB6sBB", data, offset)

    offset = header_len
    results = {
        "url": url.rstrip("\0"),
        "duration": dur,
        "bytes": size,
        "server_count": servers,
        "object_count": obj,
        "keep_alive": bool(persist),
        "max_connections": max_con,
        "max_connections_per_server": max_con_ps,
        "max_persistent_connections_per_server": max_ps_ps,
        "pipelining": bool(pipe),
        "pipelining_maxrequests": pipe_max,
        "caching": bool(cache),
        "servers": []
    }

    # extract every server in the data portion of the message
    while servers > 0:
        if len(data[offset:]) < server_len:
            print "%s: not enough data to unpack server", __file__
            return None
	# "p" pascal string could be useful here, length byte before string
        host,start_s,start_us,end_s,end_us,addr,pad1,size,pad2,obj,pad3 = struct.unpack_from("=256sQQQQ46sHiHBB", data, offset)
        offset += server_len

        server = {
            "hostname": host.rstrip("\0"),
            "address": addr.rstrip("\0"),
            "start": float("%d.%.6d" % (start_s, start_us)),
            "end": float("%d.%.6d" % (end_s, end_us)),
            "bytes": size,
            "object_count": obj,
            "objects": [],
        }

        # extract each object from this server
        object_count = obj
        while object_count > 0:
            if len(data[offset:]) < object_len:
                print "%s: not enough data to unpack object", __file__
                return None
            path,start_s,start_us,end_s,end_us,dns_s,dns_us,con_s,con_us,trans_s,trans_us,total_s,total_us,code,size,pad,con_count,pipe = struct.unpack_from("=256sQQQQQQQQQQQQII6sBB", data, offset)
            offset += object_len

            if len(data[offset:]) < cache_len:
                print "%s: not enough data to unpack cache info", __file__
                return None
            max_age,s_maxage,pad,x_cache,x_cache_lu,flags = struct.unpack_from("=ii5sbbB", data, offset)
            offset += cache_len

            # If there is only one server, with one object and that object
            # has a code of zero, then we failed to fetch anything at all.
            # Change the duration to None so the graphs properly interrupt
            # the line.
            if ( servers == 1 and len(results["servers"]) == 0 and
                    obj == 1 and code == 0 ):
                results["duration"] = None
                # TODO should we still add the object?
                #results["object_count"] = 0
                #server["object_count"] = 0
                #break

            # Append this object to the list for this server
            server["objects"].append({
                "path": path.rstrip("\0"),
                "start": float("%d.%.6d" % (start_s, start_us)),
                "end": float("%d.%.6d" % (end_s, end_us)),
                "lookup_time": float("%d.%.6d" % (dns_s, dns_us)),
                "connect_time": float("%d.%.6d" % (con_s, con_us)),
                "start_transfer_time": float("%d.%.6d" % (trans_s, trans_us)),
                "total_time": float("%d.%.6d" % (total_s, total_us)),
                "code": code,
                "bytes": size,
                "connect_count": con_count,
                "pipeline": pipe,
                "headers": {
                    "flags": {
                        "pub": bool(flags & 0x8),
                        "priv": bool(flags & 0x7),
                        "no_cache": bool(flags & 0x6),
                        "no_store": bool(flags & 0x5),
                        "no_transform": bool(flags & 0x4),
                        "must_revalidate": bool(flags & 0x3),
                        "proxy_revalidate": bool(flags & 0x2),
                    },
                    "max_age": max_age,
                    "s_maxage": s_maxage,
                    "x_cache": x_cache,
                    "x_cache_lookup": x_cache_lu,
                },
            })
            object_count -= 1

        # Add this whole server with objects to the results
        results["servers"].append(server)
        servers -= 1

    return results
