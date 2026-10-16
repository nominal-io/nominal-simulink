%% Streaming Through a Relay
% Keep the API key off the computer running the model.
%
% In proxy mode the block sends its rows as UDP packets to |nmproxy|, a relay that can run
% on another computer or on the same one. The relay holds the API key and writes the rows
% to Nominal, so the computer running the model needs only the dataset's RID and the
% relay's address. Use proxy mode when that computer shouldn't hold an API key, such as a
% shared test bench, or when it has no processing time to spare for sending data.

%% Get the Relay
% Download the |nmproxy| archive for your platform from the latest release on the
% <https://github.com/nominal-io/nominal-simulink/releases releases page>. Each archive
% holds the |nmproxy| program and |nmproxy.example.yaml|, an example configuration to
% start from.

%% Configure It
% Create |nmproxy.yaml|:
%
%   nominal:
%     base_url: "<API URL>"
%     token: "<API key>"
%
%   allowed_datasets:
%     - "<dataset RID>"
%
% The API URL and the API key are on your organization's *API keys* page in Nominal.
%
% List every dataset the relay should write to under |allowed_datasets|. The relay
% doesn't authenticate senders, so without the list anything that can reach its port can
% write to any dataset your API key can.
%
% The relay listens on UDP port 15400 on every network interface. To accept packets on
% one interface only, add |listen: "<interface address>:15400"|.

%% Start It
% From the folder holding |nmproxy| and |nmproxy.yaml|:
%
%   ./nmproxy -config nmproxy.yaml
%
% The relay logs a |listening| line with its address, and runs until you press Ctrl+C.
%
% To check that packets arrive before you involve an API key, use a configuration file
% that contains only |dry_run: true|. The relay then logs what arrives and writes nothing.

%% Point the Block at It
% In the block's dialog, set *Mode* to *Via proxy*, *Proxy host* to the relay computer's
% name or IP address in quotes, and *Proxy port* to the relay's port. *Dataset RID* and
% the channel names stay as they were. Proxy mode doesn't use |NOMINAL_TOKEN|.
%
% When the model runs, the relay logs |configuration registered| and then |session
% started|. Every 30 seconds it logs a |stats| line. In it, |rows| above 0 means data is
% reaching the relay, |gaps| counts packets lost on the way, and |write_errors| counts
% rows the relay couldn't write to Nominal.
%
% The block's own end-of-run warning covers only rows it couldn't send. Packets lost on
% the way to the relay appear only as |gaps| in the relay's log.

%% See Also
% <html><a href="nominal_stream_sink.html">Nominal Stream Sink</a> |
% <a href="troubleshooting.html">Troubleshooting</a></html>
%
% Nominal Blockset @VERSION@
