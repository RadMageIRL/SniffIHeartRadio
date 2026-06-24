# SniffIHeartRadio
SniffIHeartRadio.cpp is a standalone diagnostic utility that probes undocumented iHeartRadio API endpoints to identify and cache metadata sources for HLS streams.

# Overview

This tool serves as an active prober rather than a passive network sniffer. It is designed to reverse-engineer iHeartRadio’s undocumented "now-playing" API endpoints by programmatically testing candidate URLs and analyzing the returned data. It is intended for development use to observe API behavior and automate the discovery of working metadata endpoints for the RE-MOCT project.

# Core Functionality

API Discovery: Accepts an iHeart HLS stream URL, extracts the station token (e.g., zc####), and iterates through a list of predefined API endpoints to identify which ones return valid, actionable JSON metadata.

Diagnostic Logging: Generates a comprehensive trace file (%TEMP%\re-moct-iheartbeat.log), capturing raw and formatted JSON responses. This provides a persistent record for debugging when iHeartRadio modifies their API structure.

Identity Caching: Produces a structured "sidecar" JSON file (%TEMP%\re-moct-iheart-stations.json) that maps station tokens to confirmed metadata URLs, enabling the main application to perform lookups efficiently.

# Technical Deep Dive

# HTTP Request Process
The application utilizes the Windows WinINet API to perform network operations, chosen for its native integration with Windows environments.

Session Management: It initializes a browser-mimicking user agent (Mozilla/5.0...) and sets specific timeouts (8 seconds) to handle potential API latency.

Request Lifecycle: For each candidate endpoint, the tool performs a synchronous InternetOpenUrlA call. It requests application/json specifically to trigger the correct response format from iHeart's servers.

Resilience: The tool captures the "final URL" after redirects, which is critical for identifying if an API call has been redirected to a different regional or load-balanced host.

# JSON Heuristic Search Logic

Because iHeartRadio’s API responses can vary significantly in structure, the tool implements a recursive heuristic search function (findFirstString) to locate metadata without needing hardcoded paths:

Recursive Traversal: The function navigates through the parsed nlohmann::json object, inspecting both objects and arrays.

Case-Insensitive Matching: It converts all keys to lowercase and compares them against a priority list of known field names (e.g., artist, artistname, tracktitle, song).

Extraction: By searching for these common keys, the tool can successfully identify track metadata even if the API introduces new nesting levels or changes the schema, ensuring the prober remains robust against minor API churn.

# Environment & Dependencies

Language: C++20

Network: Windows wininet

JSON Parsing: Requires json.hpp (nlohmann/json) located in the include path.

# Build ^ Usage

Build (MSYS2 ucrt64):
g++ -std=c++20 SniffIHeartRadio.cpp -o SniffIHeartRadio.exe -lwininet
SniffIHeartRadio.exe -S https://stream.revma.ihrhls.com/zc4366/hls.m3u8

# Request-Response Flow Overview
The application follows a structured lifecycle when interacting with the iHeartRadio API, ensuring that each request is authenticated correctly and the response is captured for analysis.

Initialization: The tool sets up a persistent HINTERNET session using a browser-mimicking user agent string to ensure compatibility with iHeart's server-side request filtering.

Execution: It performs synchronous InternetOpenUrlA calls for each pre-defined API endpoint in the probes vector.

Data Capture: Each response is read in chunks to handle varying body sizes and stored in an HttpResult structure, which tracks both the raw body and the final URL after any server-side redirects.

# JSON Heuristic Processing
Because API schemas can evolve, the tool does not rely on hard-coded JSON paths. Instead, it uses a recursive traversal method to find key-value pairs.

Recursive Inspection: The findFirstString function inspects every node of the JSON hierarchy, whether it is an object or an array, ensuring that deeply nested data is not missed.

Normalization: All keys are converted to lowercase using std::tolower, allowing the prober to match keys like "ArtistName", "artist_name", or "ARTIST" identically.

Heuristic Matching: The tool checks the normalized keys against a list of known metadata indicators (e.g., {"name", "stationname", "artist", "title"}), allowing it to successfully identify data even if the API structure undergoes minor changes.
