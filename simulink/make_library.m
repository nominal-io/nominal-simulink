function make_library()
%MAKE_LIBRARY  Generate nominal_lib.slx, the library holding the sink block.
%
%   The library is generated rather than hand-authored and committed. An .slx is a
%   zipped binary, so a checked-in one cannot be reviewed in a diff, and every mask
%   change would land as an opaque blob. Generating it from this script keeps the
%   block's definition in readable source and makes the library a build artifact.
%
%   Run from this folder, or via `just library`.

    libName  = 'nominal_lib';
    blockName = 'Nominal Stream Sink';

    here = fileparts(mfilename('fullpath'));
    target = fullfile(here, [libName '.slx']);

    % Start from scratch: leaving an old copy loaded is the usual reason a mask edit
    % appears not to take.
    if bdIsLoaded(libName)
        close_system(libName, 0);
    end
    if isfile(target)
        delete(target);
    end

    new_system(libName, 'Library');
    load_system(libName);

    blockPath = [libName '/' blockName];
    add_block('built-in/S-Function', blockPath);

    set_param(blockPath, ...
        'FunctionName', 'nominal_sink_sfun', ...
        'Parameters', ['channels, datasetRid, baseUrl, sampleTime, ' ...
                       'capacityRows, timeMode, showDropped']);

    % --- mask -------------------------------------------------------------------
    %
    % Note there is deliberately no token parameter. Mask values are saved inside the
    % .slx, so a token entered here would be a credential committed to a model file.
    % The block reads NOMINAL_TOKEN from the environment instead.
    m = Simulink.Mask.create(blockPath);
    m.Description = [ ...
        'Streams the input vector to a Nominal dataset while the model runs.' newline newline ...
        'One element per channel, in the order the channel names are listed. ' ...
        'Writing happens on a background thread, so no network or TLS work occurs ' ...
        'in the model step, and no memory is allocated there either.' newline newline ...
        'If the network cannot keep up, rows are discarded and counted rather than ' ...
        'stalling the model — the output port reports the running total.' newline newline ...
        'The access token is read from the NOMINAL_TOKEN environment variable and is ' ...
        'deliberately not a parameter, so it is never saved into the model.'];

    m.addParameter('Type', 'edit', 'Name', 'channels', ...
        'Prompt', 'Channel names (comma-separated)', ...
        'Value', '''engine.rpm, engine.egt, engine.psi''');

    m.addParameter('Type', 'edit', 'Name', 'datasetRid', ...
        'Prompt', 'Dataset RID', 'Value', '''''');

    m.addParameter('Type', 'edit', 'Name', 'baseUrl', ...
        'Prompt', 'API base URL', ...
        'Value', '''https://api.gov.nominal.io/api''');

    m.addParameter('Type', 'edit', 'Name', 'sampleTime', ...
        'Prompt', 'Sample time (s) — how often to write, not the solver step', ...
        'Value', '0.001');

    m.addParameter('Type', 'edit', 'Name', 'capacityRows', ...
        'Prompt', 'Buffer capacity (rows)', 'Value', '4096');

    % Simulation time is the default: it gives a run whose time axis matches the model
    % rather than the clock, which is what an offline sim wants. Wall clock is for
    % real-time and HIL.
    % Evaluate must be on: with it off the popup hands the S-function the option text,
    % where it expects a number. On, the value is the 1-based option index, which is
    % what P_TIME_MODE reads.
    m.addParameter('Type', 'popup', 'Name', 'timeMode', ...
        'Prompt', 'Timestamps', ...
        'TypeOptions', {'Simulation time from start', 'Host wall clock'}, ...
        'Value', 'Simulation time from start', ...
        'Evaluate', 'on');

    % Off by default: Simulink treats an unconnected output as a block error, so a port
    % that is always there would force every user of a sink to wire up or terminate a
    % diagnostic most of them do not want. With it off, the block still warns at the end
    % of a run if anything was dropped, so loss is never silent.
    m.addParameter('Type', 'checkbox', 'Name', 'showDropped', ...
        'Prompt', 'Add an output port with the discarded-row count', ...
        'Value', 'off', 'Evaluate', 'on');

    m.Display = 'disp(''Nominal Stream Sink'')';

    % --- save -------------------------------------------------------------------
    set_param(libName, 'Lock', 'off');
    save_system(libName, target);
    close_system(libName, 0);

    fprintf('wrote %s\n', target);
end
