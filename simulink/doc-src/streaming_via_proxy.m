%% Streaming Through a Relay
% Move the encoding, TLS and HTTP off the machine running the model.

%% When to Use It
% In direct mode the model step stays cheap, but the block's background thread still
% pays for protobuf encoding, TLS and HTTP on the same machine. On a host already at its
% performance limit that can be too much.
%
% In proxy mode the block sends compact UDP datagrams instead, and a relay process on
% another machine does the rest. The model's host does little more than a |sendto|,
% and holds no credential.
%
%   Simulink host                     relay machine
%   +---------------+   UDP frames   +----------+   HTTPS
%   | model + block | -------------> | nmproxy  | --------> Nominal
%   +---------------+                +----------+
%     no token here                   token here

%% Run the Relay
% The relay, |nmproxy|, is in the
% <https://github.com/nominal-io/nominal-simulink nominal-simulink repository>, with an
% example configuration. The two settings that matter:
%
%   nominal:
%     token: "<your token>"
%
%   allowed_datasets:
%     - "ri.catalog.your-env.dataset.0000..."
%
% *Set |allowed_datasets|.* The datagrams are not authenticated, so anything that can
% reach the relay's port can otherwise write to any dataset its token permits. Listing
% the datasets you expect bounds the damage. The relay warns at startup if the list is
% empty.
%
% To check that frames arrive before involving real credentials, set |dry_run: true|.
% The relay then decodes and reports everything and writes nothing.

%% Point the Block at It
% Set *Mode* to |Via proxy|, then *Proxy host* and *Proxy port* to where the relay
% listens. Nothing else changes: the same channel names, dataset RID and sample time
% apply.

%% What You Give Up
% * The relay is a second process to deploy and keep running.
% * The model cannot see loss between it and the relay; the relay counts it, from
%   sequence numbers, and reports it in its log as |gaps|.
% * A relay that restarts mid-run recovers on its own within a second, because the
%   block repeats the channel layout every second.

%% See Also
% <nominal_stream_sink.html Nominal Stream Sink> |
% <troubleshooting.html Troubleshooting>
%
% Nominal Blockset @VERSION@
