%% Nominal Stream Sink
% Stream the input vector to a Nominal dataset while the model runs.
%
% <html><b>Library:</b> Nominal</html>

%% Description
% The Nominal Stream Sink block writes one row per sample period to a Nominal dataset.
% Each element of the input is one channel, in the order the channel names are listed.
%
% Writing happens on a background thread, so no network or TLS work occurs in the
% model step and no memory is allocated there: all of it is reserved when the model
% starts. If the network cannot keep up, rows are discarded and counted rather than
% stalling the model.
%
% The block has two transports, chosen by the *Mode* parameter, so moving from one to
% the other is a change of setting rather than a rewiring:
%
% * *Direct to Nominal* — this machine sends HTTPS to Nominal itself. Nothing else to
%   run.
% * *Via proxy* — this machine sends compact UDP datagrams to a relay, which does the
%   encoding and HTTPS. See <streaming_via_proxy.html Streaming Through a Relay>.

%% Ports
% *Input*
%
% A vector of doubles, one element per channel. The width must equal the number of
% channel names. To record several signals, combine them with a Mux or a Bus Creator.
%
% *Output* (optional)
%
% The running count of discarded rows. Present only when *Add an output port with the
% discarded-row count* is selected.

%% Parameters
% <html>
% <table border="1" cellpadding="6" cellspacing="0">
% <tr><th>Parameter</th><th>What it does</th></tr>
% <tr><td>Channel names (comma-separated)</td>
%     <td>Names as they appear in Nominal, for example
%         <tt>'engine.rpm, engine.egt, engine.psi'</tt>. The count sets the input port
%         width. Use <b>Detect from connected signal</b> rather than typing them.</td></tr>
% <tr><td>Detect from connected signal</td>
%     <td>Reads the names off whatever is wired into the input: a Bus Creator's element
%         names, each line into a Mux, a Bus Selector's selected signals, or otherwise
%         the signal's name or its source block's. Duplicates get a <tt>_2</tt> suffix
%         rather than collapsing two signals into one channel.</td></tr>
% <tr><td>Dataset RID</td>
%     <td>An existing Nominal dataset. The block writes points; it does not create
%         datasets.</td></tr>
% <tr><td>Mode</td>
%     <td><tt>Direct to Nominal</tt> or <tt>Via proxy</tt>. Fields the selected mode
%         ignores are greyed out.</td></tr>
% <tr><td>API base URL</td>
%     <td>Direct mode only. Must match the dataset's environment: a staging RID does
%         not resolve against the production host.</td></tr>
% <tr><td>Proxy host, Proxy port</td>
%     <td>Proxy mode only. Where the relay is listening.</td></tr>
% <tr><td>Sample time (s)</td>
%     <td>How often to write. This is decimation, not the solver step: a 1 ms solver
%         step with a 10 ms sample time writes every tenth step.</td></tr>
% <tr><td>Buffer capacity (rows)</td>
%     <td>Rows held in memory while the network drains. Reserved at model start and
%         never grown. At 1 kHz, 60000 rows is a minute of headroom.</td></tr>
% <tr><td>Timestamps</td>
%     <td><tt>Simulation time from start</tt> gives a time axis matching the model,
%         which is what an offline run wants. <tt>Host wall clock</tt> is for real-time
%         and hardware-in-the-loop.</td></tr>
% <tr><td>Add an output port with the discarded-row count</td>
%     <td>Off by default, because Simulink treats an unconnected output as an error.
%         The block warns at the end of a run whether or not this is on.</td></tr>
% </table>
% </html>

%% The Access Token
% Direct mode reads the token from the |NOMINAL_TOKEN| environment variable. Set it
% before starting MATLAB, or at the MATLAB prompt:
%
%   setenv('NOMINAL_TOKEN', '<your token>')
%
% It is deliberately not a block parameter: mask values are saved in the model file,
% so a token typed into the dialog would be a credential committed with the model.
%
% Proxy mode needs no token. The dataset RID travels in the datagrams and the
% credential stays on the relay.

%% Seeing Data Arrive Live
% A model normally runs as fast as it can, so a 60-second simulation finishes in about
% a second. To watch points appear in Nominal as the model runs, turn on *Run >
% Simulation Pacing > Enable pacing* at 1x real time.

%% Loss
% When the simulation stops, the block warns if any rows did not reach Nominal:
%
%   Warning: Nominal Stream Sink: 412 of 2001 rows did not reach Nominal
%   (2 requests failed, last HTTP status 0). Last error: Could not connect to server
%
% The number after "of" is what the model produced, so the ratio tells you whether it
% was a blip or a total failure.

%% Simulation Modes and Code Generation
% The block runs in Normal and Accelerator simulation modes.
%
% Code generation is not yet supported, so the block cannot yet be deployed to a
% Simulink Real-Time target. That support is planned.

%% See Also
% <streaming_via_proxy.html Streaming Through a Relay> |
% <troubleshooting.html Troubleshooting> |
% <nominal_blockset.html Nominal Blockset>
%
% Nominal Blockset @VERSION@
