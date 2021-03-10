/*
 * (C) 2021, Philip Prindeville
 *
 * Micro-service to handle image analysis and reporting as to whether
 * an image has been photoshopped or not.
 *
 * Rather simplistic for now, relying on XMP annotations.
 */

#include <iostream>
#include <sstream>
#include <string>
#include <cassert>

#include <stdlib.h>

#include <exempi/xmp.h>
#include <exempi/xmp++.hpp>
#include <exempi/xmpconsts.h>

#include "Poco/Net/HTTPServer.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include "Poco/Net/HTTPRequestHandlerFactory.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/Net/HTTPMessage.h"
#include "Poco/Net/ServerSocket.h"
#include "Poco/Util/ServerApplication.h"
#include "Poco/JSON/Object.h"
#include "Poco/URI.h"

using namespace Poco;
using namespace Poco::Net;
using namespace Poco::Util;
using namespace Poco::JSON;

const unsigned maxImageSize = (128 * 1024 * 1024);
const unsigned maxFilenameSize = 64;

// check a filename as containing only allowed characters (overly restrictive)
static bool sanitized(const std::string& path)
{
	for (std::string::const_iterator it = path.begin(); it != path.end(); ++it)
		if (!isalpha(*it) && !isdigit(*it) && *it != '_' && *it != '-' && *it != '.')
			return false;
	return true;
}

// check for the xmp:CreatorTool matching "Adobe Photoshop .*"
static bool is_creator_photoshop(XmpPtr xmp)
{
	XmpStringPtr prop = xmp_string_new();
	bool found, result;

	found = xmp_get_property(xmp, NS_XAP, "xmp:CreatorTool", prop, NULL);
	result = (found && !strncmp(xmp_string_cstr(prop), "Adobe Photoshop ", 16));
	xmp_string_free(prop);

	return result;
}

// check for the xmp:CreateDate and xmp:ModifyDate being in disagreement
static bool is_modifiedDate_dissimilar(XmpPtr xmp)
{
	XmpDateTime created, modified;
	bool found, equal;

	found = xmp_get_property_date(xmp, NS_XAP, "xmp:CreateDate", &created, NULL);

	if (!found)
		return false;

	found = xmp_get_property_date(xmp, NS_XAP, "xmp:ModifyDate", &modified, NULL);
	if (!found)
		return false;

	// ignore tzSign, tzHour, tzMinute, and nanoSecond for now...
	equal = (created.year == modified.year &&
		  created.month == modified.month &&
		  created.day == modified.day &&
		  created.hour == modified.hour &&
		  created.minute == modified.minute &&
		  created.second == modified.second);

	return !equal;
}

/*
 * main engine of stub server.
 *
 * borrowed shamelessly from the Poco documentation.
 */

class MyRequestHandler: public HTTPRequestHandler
{
	bool validateRequest(HTTPServerRequest& request)
	{
		URI uri(request.getURI());
		std::vector<std::string> segments;

		uri.getPathSegments(segments);

		// for now, assume that the path is the file basename
		if (segments.size() != 1)
			return false;

		// perform some validation on the name
		if (! sanitized(segments[0]))
			return false;

		// don't exceed maximum length
		if (segments[0].length() > maxFilenameSize)
			return false;

		_basename = segments[0];

		// enforce maximum file size test
		if (request.getContentLength64() == HTTPMessage::UNKNOWN_CONTENT_LENGTH ||
		    request.getContentLength64() > maxImageSize)
			return false;

		// we don't handle multipart/form-data for now
		if (request.getContentType() != "application/x-www-form-urlencoded")
			return false;

		return true;
	}

	void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response)
	{
		Application& app = Application::instance();
		Object obj(JSON_PRESERVE_KEY_ORDER), tests(JSON_PRESERVE_KEY_ORDER);

		// could have used a try / throw / catch but that's a lot
		// of overhead for what's effectively a "goto cleanup;"
		// so, using a "do { } while (0);" instead with a "break" to
		// jump to the common cleanup coda

		do {
			// mark as invalid, and we'll flip this once
			// we're far enough along to process the
			// request in a meaningful way.
			obj.set("is_valid", false);

			app.logger().information("Request from %s of %Ld bytes", request.clientAddress().toString(), request.getContentLength64());

			response.setChunkedTransferEncoding(true);
			response.setContentType("application/json");

			// sets _basename as a side-effect
			if (!validateRequest(request))
				break;

			obj.set("name", _basename);

			// this is just painful to look at.

			char tmpfile[24];
			snprintf(tmpfile, sizeof(tmpfile), "/tmp/picserver.XXXXXX");
			(void)mktemp(tmpfile);

			// failure to generate a unique name
			if (*tmpfile == '\0')
				break;

			FILE *tf = fopen(tmpfile, "wb");

			int cc;
			while ((cc = request.stream().get()) != EOF)
				// _post_data.push_back(cc);
				fputc(cc, tf);

			fclose(tf);

			// true confession: I started to use the exempi
			// library because it was packaged for RHEL/EPEL
			// and it was derived (forked) from the Adobe SDK.
			// who knew that it's such a mess?  you can't process
			// blobs of memory (i.e. in-memory images), you can't
			// even process images wrapped via fmemopen() in
			// a FILE *.
			// if I had less sunken time in this, I would search
			// for a less broken library and use that.
			// that said, there don't seem to be a lot of choices
			// for RHEL/EPEL.

			xmp_init();

			// the horror, the horror!!
			XmpFilePtr xf = xmp_files_open_new(tmpfile, XMP_OPEN_READ);

			// resource exhaustion? we would log something here
			// if this were a production service
			if (xf == NULL)
				break;

			// this could fail because of malformed input
			// or because of low resources. in a production
			// service, and with a better API, we could
			// easily determine this and log it when
			// needed.
			XmpPtr xmp = xmp_files_get_new_xmp(xf);

			if (xmp == NULL) {
				xmp_files_free(xf);
				break;
			}

			// retrieve file format
			XmpFileType type;
			xmp_files_get_file_info(xf, NULL, NULL, &type, NULL);

			// only accept JPEG files
			if (type != XMP_FT_JPEG)
				break;

			// at this point, we're able to call libexempi
			// and perform analysis of the contents

			obj.set("is_valid", true);

			// do a battery of tests and append results

			// test for xmp::CreatorTool as Adobe Photoshop .*
			tests.set("creator_tool_is_photoshop", is_creator_photoshop(xmp));

			// test for xmp:CreateDate mismatch :ModifyDate
			tests.set("create_modify_mismatch", is_modifiedDate_dissimilar(xmp));

			// test for stEvt:softwareAgent as Adobe Photoshop .*
			// ... except I can't figure out the namespace to do
			// so in libexempi...

			// look for an xmpMM:History array indicative of
			// multiple stages of processing having been done.

			// and probably need other tests, but this is as
			// far as I got.

			xmp_free(xmp);
			xmp_terminate();

			obj.set("tests", tests);
		} while (0);

		obj.stringify(response.send(), 2);
	}

private:
	std::string _basename;
};

class MyRequestHandlerFactory: public HTTPRequestHandlerFactory
{
	HTTPRequestHandler* createRequestHandler(const HTTPServerRequest&)
	{
		return new MyRequestHandler;
	}
};

class WebServerApp: public ServerApplication
{
	void initialize(Application& self)
	{
		loadConfiguration();
		ServerApplication::initialize(self);
	}

	int main(const std::vector<std::string>&)
	{
		UInt16 port = static_cast<UInt16>(config().getUInt("port", 8080));

		// we should bind to localhost if we're being proxied
		// by an Apache, etc. server applying access controls, etc.

		HTTPServer srv(new MyRequestHandlerFactory, port);
		srv.start();
		logger().information("HTTP Server started on port %hu.", port);
		waitForTerminationRequest();
		logger().information("Stopping HTTP Server...");
		srv.stop();

		return Application::EXIT_OK;
	}
};

POCO_SERVER_MAIN(WebServerApp)

// vim: ts=8
