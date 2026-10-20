%% Nominal Stream Sink
% Stream signals to a Nominal dataset while the model runs
%
% <html><b>Library:</b> Nominal</html>

%% Description
% The Nominal Stream Sink block writes the signals wired into it to an existing Nominal
% dataset while the model runs. Each element of the input is one channel, named by
% *Channel names (comma-separated)* in the same order.
%
% While the model runs, the block never waits for the network. If sending falls behind,
% it drops rows rather than slowing the model, and warns at the end of the run with how
% many.
%
% *Mode* chooses how the data leaves the model:
%
% * *Direct to Nominal* sends it over HTTPS from the computer running the model, using
%   the API key and URL in a profile in Nominal's config file.
% * *Via proxy* sends it over UDP to |nmproxy|, a relay that holds the API key and
%   writes to Nominal. See <streaming_via_proxy.html Streaming Through a Relay>.
%
% The block's label on the canvas shows the mode it sends in: *(direct)* or *(proxy)*.

%% Examples
% <matlab:open_system(fullfile(nominal_root,'examples','nominal_stream_example.slx')) Open
% the example model>. It streams three signals through a relay, and runs without an API
% key or a relay.

%% Ports
% *Input*
%
% *Port_1* — the signals to record, as a vector
%
% A vector of doubles, one element per channel, whose width equals the number of channel
% names. To record several signals, wire them into a Mux or a Bus Creator. Signals of
% other types need a Data Type Conversion block set to |double|.
%
% *Output*
%
% *Port_1* — rows dropped so far, as a scalar
%
% The number of rows dropped so far in the current run, as a double. The port exists
% only when *Add an output port with the discarded-row count* is selected.

%% Parameters
% Text parameters take a MATLAB expression, so enter text in single or double quotes, or
% the name of a variable that holds the text. No parameter can change while the model
% runs. The dialog greys out *Profile* and *Profile file* in proxy mode, *Proxy host*
% and *Proxy port* in direct mode, and *Run title* and *Ref name* when *Create run* is
% *None*.
%
% To set a parameter from code, use its programmatic name. A text value keeps its quotes
% inside the string:
%
%   set_param(blk, 'datasetRid', '''<dataset RID>''')
%
% *Channel names (comma-separated)* — the channel names, in input order
%
% The number of names sets the width of the block's input. Spaces around each name are
% ignored, and so are empty entries. *Detect from connected signal* fills them in from
% the model.
%
% Programmatic name: |channels|. Default: |'engine.rpm, engine.egt, engine.psi'|.
%
% *Detect from connected signal* — fill in the channel names from the model
%
% Reads the names from whatever is wired into the block, without compiling the model:
%
% * From a Mux, each input line's name, or the name of the block the line comes from if
%   the line has none. An input fed by a Bus Creator, Bus Selector or another Mux gives
%   the names described below, and an input with nothing connected is named by position:
%   |in1|, |in2| and so on.
% * From a Bus Creator, the bus element names.
% * From a Bus Selector, every selected signal when *Output as virtual bus* is on, or the
%   signal on the port wired to the block when it is off.
% * From any other block, the line's name, or the name of the block it comes from.
%
% When names repeat, the second gets |_2| added, the third |_3|, and so on, skipping any
% name already in the list. A vector signal that carries one name gives one channel
% name, and the model then fails with a port width mismatch. Wire its elements into a
% Mux as named lines, or type the names.
%
% *Dataset RID* — the dataset to write to
%
% The RID of an existing Nominal dataset. The block does not create datasets. Required.
%
% Programmatic name: |datasetRid|. Default: |''|.
%
% *Tags (key and value pairs)* — tags on every channel
%
% Tags keep the runs of one model apart in a dataset they share. Enter a two-column
% string array with one row per tag, such as |["model" "A"; "execution-id" "7"]|, a cell
% array of the same shape, a |dictionary| or a |containers.Map|. A value can be a
% number. Keys can't be empty or repeat.
%
% Programmatic name: |tags|. Default: |[]|, no tags.
%
% *Execution ID* — the ID that picks out each run
%
% Sent as the tag |execution-id| on every channel. |'auto'| makes a new ID when each run
% starts: the run's start time in UTC, then six hexadecimal digits that differ between
% runs starting in the same second. Text or a number sets the ID yourself, such as a
% variable a script sets for each run with |Simulink.SimulationInput|. |''| sends no
% execution ID. Tags can't have an |execution-id| key unless this field is empty.
%
% Programmatic name: |executionId|. Default: |'auto'|.
%
% *Mode* — how the data leaves the model
%
% *Direct to Nominal* or *Via proxy*, as described above.
%
% Programmatic name: |mode|. Values: |'Direct to Nominal'| (default), |'Via proxy'|.
%
% *Profile* — where the API key and URL come from
%
% Direct mode only. The profile in Nominal's config file to read the API key and the
% instance's REST API base URL from, as its |token| and |base_url|. It is the same file
% Nominal's other clients read.
%
% Programmatic name: |profile|. Default: |'default'|.
%
% *Profile file* — the config file, when it is somewhere else
%
% Direct mode only. Empty reads the standard location: |~/.config/nominal/config.yml|,
% or |%USERPROFILE%\.config\nominal\config.yml| on Windows.
%
% Programmatic name: |profileFile|. Default: |''|.
%
% *Create run* — a run in Nominal for each execution
%
% *One per execution* creates a run when each run of the model starts, so it can be
% watched live, and sets its end once the last row is sent. The run shows the dataset's
% points with this execution's tags, the execution ID included. *One per model* shares one
% run between every execution with the same title: the first creates it, later ones
% extend its end, and it shows the points with the tags in *Tags*. Finding the run means
% searching every run whose title contains this one, so the more runs have similar
% titles, the longer an execution takes to start. When a run can't be
% created or ended, the block warns and the rows still go to the dataset. In proxy mode
% the relay creates and ends the run with its own profile, and warns in its log instead.
%
% Programmatic name: |createRun|. Values: |'None'| (default), |'One per execution'|,
% |'One per model'|.
%
% *Run title* — the run's title
%
% Empty makes one from the model's name: one per execution adds the execution ID, as in
% |Model A, execution 7|, and one per model uses the name alone.
%
% Programmatic name: |runTitle|. Default: |''|.
%
% *Ref name* — the name the dataset goes under in the run
%
% Workbooks find a channel by its ref name and channel name together, so use the ref
% name your other runs use.
%
% Programmatic name: |refName|. Default: |'default'|.
%
% *Proxy host* — where the relay runs
%
% Proxy mode only. The name or IP address of the computer running |nmproxy|.
%
% Programmatic name: |proxyHost|. Default: |'127.0.0.1'|.
%
% *Proxy port* — the relay's UDP port
%
% Proxy mode only. The port |nmproxy| listens on, from 1 to 65535.
%
% Programmatic name: |proxyPort|. Default: |15400|.
%
% *Sample time (s) — how often to write, not the solver step* — seconds between rows
%
% How often the block takes a row from its input. It is separate from the solver step:
% with a 1 ms fixed step and a sample time of 0.01, the block writes on every tenth
% step. A value of 0 or less, including -1, uses 0.001.
%
% Programmatic name: |sampleTime|. Default: |0.001|.
%
% *Buffer capacity (rows)* — rows held while they wait to be sent
%
% Rows arriving while the buffer is full are dropped. In a model running in real time at
% 1000 rows a second, 60000 rows holds a minute. The buffer is reserved when the run
% starts, so a larger one uses more memory. When the run ends, the block spends up to 15
% seconds sending what is left in the buffer, and counts rows still unsent as dropped. A
% value of 0 uses 4096.
%
% Programmatic name: |capacityRows|. Default: |4096|.
%
% *Timestamps* — the time on each row
%
% *Simulation time from start* stamps each row with the time the run started plus the
% row's simulation time, so the points keep the model's time axis however fast the
% simulation runs. *Directly record simulation time (t0=0)* stamps each row with its
% simulation time alone, so every run starts at the same instant, the Unix epoch, and
% runs overlay without shifting. The execution ID keeps the runs apart in the dataset.
% *Host wall clock* stamps each row with the computer's clock when the block writes
% it. Use it when the model runs in real time alongside hardware.
%
% Programmatic name: |timeMode|. Values: |'Simulation time from start'| (default),
% |'Directly record simulation time (t0=0)'|, |'Host wall clock'|.
%
% *Add an output port with the discarded-row count* — show drops while the model runs
%
% Adds the output port described under Ports.
%
% Programmatic name: |showDropped|. Values: |'off'| (default), |'on'|.

%% Environment Variable
% The block reads this when a run starts, in direct mode only.
%
% *NOMINAL_CA_BUNDLE* — the path to a PEM file of CA certificates to verify Nominal's
% HTTPS certificate with. Set it when your Nominal instance or your network uses
% certificates that the operating system doesn't trust.

%% Block Characteristics
% <html>
% <table border="1" cellpadding="6" cellspacing="0">
% <tr><td>Data Types</td><td>double</td></tr>
% <tr><td>Direct Feedthrough</td><td>yes</td></tr>
% <tr><td>Multidimensional Signals</td><td>no</td></tr>
% <tr><td>Variable-Size Signals</td><td>no</td></tr>
% <tr><td>Zero-Crossing Detection</td><td>no</td></tr>
% </table>
% </html>

%% Extended Capabilities
% *C/C++ Code Generation*
%
% Not supported. The block runs in Normal and Accelerator simulation modes.

%% Version History
% *Introduced in Nominal Blockset 0.1.0*

%% See Also
% <html><a href="streaming_via_proxy.html">Streaming Through a Relay</a> |
% <a href="troubleshooting.html">Troubleshooting</a> |
% <a href="nominal_blockset.html">Nominal Blockset</a></html>
%
% Nominal Blockset @VERSION@
