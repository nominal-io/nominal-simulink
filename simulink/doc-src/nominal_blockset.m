%% Nominal Blockset
% Stream signals from a running Simulink model into a Nominal dataset.
%
% The blockset has one block, *Nominal Stream Sink*. It writes the signals wired into it
% to a Nominal dataset while the model runs, one channel per signal. While the model
% runs, the block never waits for the network: if sending falls behind, it drops rows
% rather than slowing the model, and says how many at the end of the run.

%% Blocks
% <html>
% <table border="1" cellpadding="6" cellspacing="0">
% <tr><td><a href="nominal_stream_sink.html">Nominal Stream Sink</a></td>
%     <td>Stream signals to a Nominal dataset while the model runs, directly over HTTPS
%         or through a relay</td></tr>
% </table>
% </html>

%% Getting Started
% You need an existing dataset in Nominal and its RID, and a profile in Nominal's config
% file holding your API key and your instance's API URL. A computer already set up for
% another Nominal client, such as the Python client, has one. Otherwise follow API keys
% setup in the Nominal docs.
%
% # Open the Library Browser, expand *Nominal*, and drag *Nominal Stream Sink* into your
%   model. Typing |Nominal| on the canvas also finds it.
% # Wire the signals you want to record into a Mux, and the Mux into the block.
% # Double-click the block and click *Detect from connected signal* to fill in the
%   channel names.
% # Enter the dataset's RID in *Dataset RID*, in quotes.
% # Run the model. The block reads the profile named |default|.
%
% The block reads the API key from the profile so that it is never saved in the model
% file.
%
% <matlab:open_system(fullfile(nominal_root,'examples','nominal_stream_example.slx')) Open
% the example model> to see the block in a model without setting anything up. It sends
% through a relay, and runs without an API key or a relay.

%% Topics
% * <nominal_stream_sink.html Nominal Stream Sink>: every parameter, its ports, and what
%   the block does when sending falls behind.
% * <streaming_via_proxy.html Streaming Through a Relay>: keep the API key off the
%   computer running the model.
% * <troubleshooting.html Troubleshooting>: what to do when data doesn't arrive.
%
% Nominal Blockset @VERSION@
