%% Troubleshooting
% What to check when data does not arrive.

%% No Data Appears, and No Error
% Check that *API base URL* matches the dataset's environment. A staging RID against
% the production host is the usual cause.

%% A Warning at the End of the Run
%   Warning: Nominal Stream Sink: 412 of 2001 rows did not reach Nominal
%   (2 requests failed, last HTTP status 0). Last error: Could not connect to server
%
% The block reports loss when the simulation stops, whether or not the output port is
% enabled. The last error names the cause. HTTP status 0 means no response arrived at
% all: check the URL, and that this machine can reach it.

%% Rows Dropped With No Request Errors
% The buffer filled: the model produced rows faster than the network drained them.
% Raise *Buffer capacity*, increase *Sample time* to write less often, or move to
% <streaming_via_proxy.html proxy mode>.

%% Nothing Reaches the Relay
% Confirm the port matches on both sides, and that the relay listens somewhere this
% machine can reach: |listen: "127.0.0.1:15400"| accepts only from its own machine.
% Check the relay's log for |bad_frames| or |wrong_version|. There is no channel back
% to the block, so a version mismatch otherwise looks the same as a network fault.

%% "Undefined function" or "S-Function does not exist"
% The blockset's folders are not on the MATLAB path. Installing the toolbox puts them
% there. If the blockset is used from a copy of its source instead, run
% |nominal_setup| from that copy.

%% See Also
% <nominal_stream_sink.html Nominal Stream Sink> |
% <streaming_via_proxy.html Streaming Through a Relay>
%
% Nominal Blockset @VERSION@
