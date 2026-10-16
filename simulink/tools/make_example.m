function make_example()
%MAKE_EXAMPLE  Generate nominal_stream_example.slx, the model the documentation opens.
%
%   Generated for the same reason as the library: an .slx cannot be reviewed in a diff.
%   Run it with the oldest supported release, after make_library, as `just example`
%   does.
%
%   The model runs as it is, on any machine. It streams through a relay, which needs no
%   token, and keeps running whether or not a relay is listening, so opening it and
%   pressing Run never ends in an error about credentials or connectivity.

    model = 'nominal_stream_example';

    blockset = fullfile(fileparts(fileparts(mfilename('fullpath'))), 'blockset');
    outDir   = fullfile(blockset, 'examples');
    target   = fullfile(outDir, [model '.slx']);

    addpath(fullfile(blockset, 'library'), fullfile(blockset, 'script'), ...
            fullfile(blockset, 'mex'));
    load_system('nominal_lib');

    if bdIsLoaded(model)
        close_system(model, 0);
    end
    if ~isfolder(outDir)
        mkdir(outDir);
    end
    if isfile(target)
        delete(target);
    end

    new_system(model);
    load_system(model);

    % Three signals of different shapes, so the plot in Nominal shows three distinct
    % channels rather than three copies of one.
    add_block('simulink/Sources/Sine Wave', [model '/Sine'], ...
              'Amplitude', '1', 'Frequency', '2*pi*0.5', 'Position', [40 30 80 70]);
    add_block('simulink/Sources/Ramp', [model '/Ramp'], ...
              'Slope', '0.1', 'Position', [40 100 80 140]);
    add_block('simulink/Sources/Pulse Generator', [model '/Pulse'], ...
              'Period', '2', 'PulseWidth', '25', 'Position', [40 170 80 210]);
    add_block('simulink/Signal Routing/Mux', [model '/Mux'], ...
              'Inputs', '3', 'Position', [180 25 185 215]);

    sink = [model '/Nominal Stream Sink'];
    add_block('nominal_lib/Nominal Stream Sink', sink, 'Position', [260 90 400 150]);

    names = {'example.sine', 'example.ramp', 'example.pulse'};
    srcs  = {'Sine', 'Ramp', 'Pulse'};
    for i = 1:3
        h = add_line(model, [srcs{i} '/1'], sprintf('Mux/%d', i));
        set_param(h, 'Name', names{i});
    end
    add_line(model, 'Mux/1', 'Nominal Stream Sink/1');

    % Filled in by the same code as the dialog's Detect button, which is how a user
    % would do it.
    nominal_sink_autonames(sink, true);

    set_param(sink, ...
        'mode',       'Via proxy', ...
        'proxyHost',  '''127.0.0.1''', ...
        'proxyPort',  '15400', ...
        'datasetRid', '''ri.catalog.example.dataset.replace-me''', ...
        'sampleTime', '0.01');
    nominal_sink_modecb(sink);

    set_param(model, 'StopTime', '10', 'SolverType', 'Fixed-step', 'FixedStep', '0.01');

    note = Simulink.Annotation(model, sprintf([ ...
        'Streams three signals to a Nominal dataset.\n\n' ...
        'As shipped it sends to a relay at 127.0.0.1:15400 and needs no token. With no\n' ...
        'relay listening the datagrams are discarded and the model still runs.\n\n' ...
        'To write to Nominal directly: set Mode to Direct to Nominal, enter your\n' ...
        'dataset RID, and set NOMINAL_TOKEN before running.']));
    note.Position = [40 250 520 340];

    save_system(model, target);
    close_system(model, 0);
    close_system('nominal_lib', 0);

    fprintf('wrote %s\n', target);
end
