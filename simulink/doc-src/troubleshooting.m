%% Troubleshooting
% What to do when data doesn't arrive in Nominal.

%% An Error When the Model Starts
% The block's errors start with |Nominal Stream Sink:| and say what to change. Two come
% up on a first run:
%
% * |NOMINAL_TOKEN is not set| means MATLAB has no API key. Run
%   |setenv('NOMINAL_TOKEN', '<API key>')| in the same MATLAB session, then run the model
%   again.
% * A message that a field |must be text in quotes|, or Simulink reporting that it can't
%   evaluate a parameter, means a text value is missing its quotes.

%% A Warning When the Run Ends
% The block warns when rows didn't reach Nominal, whether or not its output port is on:
%
%   Nominal Stream Sink: execution <ID>: <dropped> of <total> rows did not reach
%   Nominal (<cause>). Last error: <reason>
%
% |<ID>| is the run's *Execution ID*, left out when the block sends none. The second
% number is every row the model produced in the run. What to do depends on the cause, in
% the parentheses:
%
% * |the buffer filled faster than the network drained it|: rows arrived faster than
%   they could be sent. Raise *Buffer capacity (rows)*, raise the sample time to write
%   less often, or switch to <streaming_via_proxy.html proxy mode>. For a simulation that
%   runs faster than real time, a buffer that holds every row of the run also works.
% * |requests failed, the last with HTTP status| followed by a number: Nominal refused
%   the rows, and the |Last error| gives its reason. Check that *API base URL* is the API
%   URL of the instance the dataset belongs to, that *Dataset RID* is right, and that the
%   API key is current.
% * |requests failed, the last with no response|: the computer couldn't reach *API base
%   URL*. Check the URL and the network.
% * |packets could not be sent to the relay|: check that |nmproxy| is running at *Proxy
%   host* and *Proxy port*.

%% Nothing Arrives Through the Relay
% Check the relay's log. A |stats| line with |rows=0| means packets aren't reaching it:
% check that its |listen| address accepts packets from the computer running the model,
% and that a firewall allows UDP on the port. |wrong_version| above 0 means the blockset
% and |nmproxy| come from different releases. A dataset missing from |allowed_datasets|
% is refused, with a warning that names it.

%% "Undefined function" or "S-Function does not exist"
% The blockset's folders aren't on the MATLAB path. Check that the Nominal Blockset is
% installed for the release of MATLAB you're running, and install it again if it isn't.

%% See Also
% <html><a href="nominal_stream_sink.html">Nominal Stream Sink</a> |
% <a href="streaming_via_proxy.html">Streaming Through a Relay</a></html>
%
% Nominal Blockset @VERSION@
