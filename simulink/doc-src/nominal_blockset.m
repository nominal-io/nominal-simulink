%% Nominal Blockset
% Stream signals from a running Simulink model into a Nominal dataset.
%
% The blockset holds one block, *Nominal Stream Sink*. Wire the signals you want
% recorded into it, give it a dataset, and the samples arrive in Nominal while the model
% runs.
%
% The block does almost nothing in the model step: it copies one row into a buffer
% reserved when the model starts, and a background thread does the encoding, TLS and
% HTTP. If the network cannot keep up it discards rows and tells you, rather than
% stalling the model. Stopping a simulation that is driving real hardware is worse than
% losing telemetry.

%% Blocks
% <html>
% <table border="1" cellpadding="6" cellspacing="0">
% <tr><td><a href="nominal_stream_sink.html">Nominal Stream Sink</a></td>
%     <td>Stream the input vector to a Nominal dataset, directly over HTTPS or through
%         a relay</td></tr>
% </table>
% </html>

%% Getting Started
% # In the Library Browser, open *Nominal* and drag *Nominal Stream Sink* into your
%   model.
% # Wire the signals you want recorded into its input, through a Mux or a Bus Creator.
% # Double-click the block and click *Detect from connected signal* to fill in the
%   channel names.
% # Enter the *Dataset RID* of an existing Nominal dataset.
% # Set the access token in the environment, then run the model:
%
%   setenv('NOMINAL_TOKEN', '<your token>')
%
% The token is deliberately not a block parameter. Mask values are saved in the model
% file, so a token typed into the dialog would be a credential committed with the model.
%
% <matlab:open_system(fullfile(nominal_root,'examples','nominal_stream_example.slx')) Open
% the example model>, which runs as it is: it streams through a relay, which needs no
% token, and keeps running when no relay is listening.

%% Topics
% * <nominal_stream_sink.html Nominal Stream Sink> — every parameter, and how the block
%   behaves
% * <streaming_via_proxy.html Streaming Through a Relay> — for hosts with no headroom
%   to spare, or that should hold no credential
% * <troubleshooting.html Troubleshooting>
%
% Nominal Blockset @VERSION@
