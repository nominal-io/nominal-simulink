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
                       'capacityRows, timeMode, showDropped, ' ...
                       'mode, proxyHost, proxyPort']);

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

    % Typing the names by hand is tedious and quietly error-prone: the list sets the
    % input port width, so one name too many or too few misaligns every column after
    % it. This reads them off whatever is wired in.
    %
    % A dialog control rather than a parameter — a button holds no value, and
    % addParameter rejects the type.
    detect = m.addDialogControl('pushbutton', 'detectNames');
    detect.Prompt   = 'Detect from connected signal';
    detect.Callback = 'nominal_sink_detect(gcb);';

    m.addParameter('Type', 'edit', 'Name', 'datasetRid', ...
        'Prompt', 'Dataset RID', 'Value', '''''');

    % Direct sends HTTPS from this machine; proxy sends compact datagrams to a relay
    % that does the protobuf, TLS and HTTP instead. One block rather than two, because
    % the expected journey is direct until the host runs out of headroom and then proxy
    % — and that should be a dropdown, not a re-wire.
    m.addParameter('Type', 'popup', 'Name', 'mode', ...
        'Prompt', 'Mode', ...
        'TypeOptions', {'Direct to Nominal', 'Via proxy'}, ...
        'Value', 'Direct to Nominal', 'Evaluate', 'on');

    m.addParameter('Type', 'edit', 'Name', 'baseUrl', ...
        'Prompt', 'API base URL', ...
        'Value', '''https://api.gov.nominal.io/api''');

    m.addParameter('Type', 'edit', 'Name', 'proxyHost', ...
        'Prompt', 'Proxy host', 'Value', '''127.0.0.1''');

    m.addParameter('Type', 'edit', 'Name', 'proxyPort', ...
        'Prompt', 'Proxy port', 'Value', '15400');

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

    % --- dialog logic ------------------------------------------------------------
    %
    % Grey out whichever fields the chosen mode ignores, so the dialog cannot suggest
    % that a proxy host matters in direct mode or that an API URL matters in proxy mode.
    % A visible-but-inert field is a standing invitation to fill it in and wonder why
    % nothing changed.
    %
    % The callback lives in its own file and works by parameter *name*. The positional
    % MaskEnables array would do the same job but breaks silently the moment a field is
    % reordered, and this dialog is going to grow.
    m.getParameter('mode').Callback = 'nominal_sink_modecb(gcb);';

    % Show the transport on the canvas. This is what a separate proxy block would have
    % bought us, without the duplication: you can still see at a glance which way a
    % model streams.
    m.Display = [ ...
        'if strcmp(get_param(gcb,''mode''),''Via proxy'')' newline ...
        '    disp(sprintf(''Nominal Stream Sink\n(proxy)''));' newline ...
        'else' newline ...
        '    disp(sprintf(''Nominal Stream Sink\n(direct)''));' newline ...
        'end'];

    % Apply it once now, so a freshly dragged block starts with the right fields greyed
    % rather than waiting for the user to touch the popup.
    nominal_sink_modecb(blockPath);

    % --- save -------------------------------------------------------------------
    set_param(libName, 'Lock', 'off');
    save_system(libName, target);
    close_system(libName, 0);

    fprintf('wrote %s\n', target);
end
