function make_library()
%MAKE_LIBRARY  Generate nominal_lib.slx, the library holding the sink block.
%
%   The library is generated rather than hand-authored and committed. An .slx is a
%   zipped binary, so a checked-in one cannot be reviewed in a diff, and every mask
%   change would land as an opaque blob. Generating it from this script keeps the
%   block's definition in readable source and makes the library a build artifact.
%
%   Run it with the oldest MATLAB the blockset supports. A library opens only in the
%   release that saved it or a later one, so whichever release runs this sets the floor
%   for everyone who installs the blockset.
%
%   Run via `just library`.

    libName  = 'nominal_lib';
    blockName = 'Nominal Stream Sink';

    blockset = fullfile(fileparts(fileparts(mfilename('fullpath'))), 'blockset');
    target   = fullfile(blockset, 'library', [libName '.slx']);
    version  = blocksetVersion();

    % The S-function must be resolvable, or Simulink cannot run mdlInitializeSizes and
    % the block keeps the default one-in-one-out shape — which then gets saved, so every
    % block dragged out shows an output port its checkbox says it does not have.
    addpath(fullfile(blockset, 'library'), fullfile(blockset, 'script'), ...
            fullfile(blockset, 'mex'));
    if isempty(which('nominal_sink_sfun'))
        error('nominal:library', ...
              ['nominal_sink_sfun is not on the path. Build it first (`just build`); ' ...
               'without it the block cannot size its ports.']);
    end

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

    % Unlocked up front, not just before saving. A locked library silently ignores
    % set_param on its blocks, which is enough to make the port-sizing step below look
    % like it ran when it did not.
    set_param(libName, 'Lock', 'off');

    % --- library metadata -------------------------------------------------------
    %
    % The Library Browser lists a library only if this is on when it is saved. slblocks.m
    % beside the .slx is the other half.
    set_param(libName, 'EnableLBRepository', 'on');

    % Pinned rather than left to auto-increment. The library is rebuilt from nothing on
    % every run, so an auto-incremented version would read 1.0 forever, and forwarding
    % table entries keyed on a version would have nothing to compare against.
    set_param(libName, 'ModelVersionFormat', sprintf('%d.%d', version(1), version(2)));

    % Renames, moves and parameter changes are carried across to users' saved models by
    % this table. It is empty until the first such change ships; see forwarding_table.
    set_param(libName, 'ForwardingTable', forwarding_table());

    % Instances cannot have their link disabled. A disabled link is a private fork of the
    % block that silently stops receiving fixes, which for a block that talks to a server
    % is how a protocol change strands someone.
    set_param(libName, 'LockLinksToLibrary', 'on');

    blockPath = [libName '/' blockName];
    add_block('built-in/S-Function', blockPath);

    % Seeded with literal values, not the mask variable names.
    %
    % There is a chicken and egg here: Parameters has to name mask variables, but the
    % mask cannot be created until the block exists, and the moment FunctionName is set
    % Simulink runs mdlInitializeSizes — which fails if the parameter count does not
    % match, or if the names do not resolve. So give it real values now, and swap in the
    % variable names once the mask is built.
    %
    % Getting this wrong is not loud: the block simply keeps the default S-Function
    % shape of one input and one output, that shape is saved, and every block dragged
    % out shows an output port its own checkbox says it does not have.
    set_param(blockPath, ...
        'FunctionName', 'nominal_sink_sfun', ...
        'Parameters', [ ...
            '''engine.rpm, engine.egt, engine.psi'', ' ...   % channels
            '''''' ', ' ...                                  % datasetRid
            '''https://api.gov.nominal.io/api'', ' ...       % baseUrl
            '0.001, ' ...                                    % sampleTime
            '4096, ' ...                                     % capacityRows
            '1, ' ...                                        % timeMode
            '0, ' ...                                        % showDropped
            '1, ' ...                                        % mode
            '''127.0.0.1'', ' ...                            % proxyHost
            '15400']);                                       % proxyPort

    % --- mask -------------------------------------------------------------------
    %
    % Note there is deliberately no token parameter. Mask values are saved inside the
    % .slx, so a token entered here would be a credential committed to a model file.
    % The block reads NOMINAL_TOKEN from the environment instead.
    m = Simulink.Mask.create(blockPath);

    % The type titles the dialog, and is what nominal_help keys a block's page on.
    m.Type = blockName;

    % Simulink runs the first line of the help text when it is an eval(...) command, which
    % lets the page be found at click time. The install folder is unknown here, and differs
    % between an installed toolbox and a clone. The path fix runs first because Help is
    % one more callback that fails when the block's folders are off the path.
    m.Help = ['eval(''' strrep([pathFixPrefix() 'nominal_help(gcb);'], '''', '''''') ''')'];

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
    detect.Callback = [pathFixPrefix() 'nominal_sink_detect(gcb);'];

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
    m.getParameter('mode').Callback = [pathFixPrefix() 'nominal_sink_modecb(gcb);'];

    % Show the transport on the canvas. This is what a separate proxy block would have
    % bought us, without the duplication: you can still see at a glance which way a
    % model streams.
    m.Display = [ ...
        'if strcmp(get_param(gcb,''mode''),''Via proxy'')' newline ...
        '    disp(sprintf(''Nominal Stream Sink\n(proxy)''));' newline ...
        'else' newline ...
        '    disp(sprintf(''Nominal Stream Sink\n(direct)''));' newline ...
        'end'];

    % Runs on every compile, so the S-function resolves even when the model was opened
    % without the block's folders on the path. Callbacks alone were not enough: a
    % missing MEX only shows up when the model is run.
    m.Initialization = pathFixPrefix();

    % Now that every mask parameter exists, point the S-function at them. Doing this
    % last is what lets mdlInitializeSizes evaluate real values and size the ports to
    % match the dialog, rather than leaving the default one-in-one-out shape.
    %
    % The text ones go through convertStringsToChars so that "double quotes" work as
    % well as 'single'. A string reaches a C S-function as something it cannot read as
    % text; it used to read as empty, which for the URL meant silently using the default
    % instance. Not char(), which would turn a mistyped number into a character and hide
    % the mistake from the S-function's own check.
    set_param(blockPath, 'Parameters', ...
        ['convertStringsToChars(channels), convertStringsToChars(datasetRid), ' ...
         'convertStringsToChars(baseUrl), sampleTime, capacityRows, timeMode, ' ...
         'showDropped, mode, convertStringsToChars(proxyHost), proxyPort']);


    % Apply it once now, so a freshly dragged block starts with the right fields greyed
    % rather than waiting for the user to touch the popup.
    nominal_sink_modecb(blockPath);

    % Assert the block matches its own dialog before saving it.
    %
    % This failure mode is silent and cost several rounds to find: the block keeps the
    % default S-Function shape whenever mdlInitializeSizes cannot run, that shape is
    % saved, and the only symptom is a model refusing to run with an error pointing at
    % the block rather than at the library that produced it.
    ports = get_param(blockPath, 'Ports');
    if numel(ports) < 2 || ports(2) ~= 0
        error('nominal:library', ...
              ['the block reports %d output(s) with showDropped off, so its ports did ' ...
               'not track its parameters. Blocks dragged from this library would not ' ...
               'run.'], ports(2));
    end

    % Quick Insert: typing any of these on the canvas offers the block.
    set_param(blockPath, 'BlockKeywords', ["Nominal", "stream", "telemetry"]);

    % --- save -------------------------------------------------------------------
    set_param(libName, 'Lock', 'off');
    save_system(libName, target);
    close_system(libName, 0);

    fprintf('wrote %s, version %d.%d (%d in, %d out)\n', ...
            target, version(1), version(2), ports(1), ports(2));
end

function v = blocksetVersion()
%BLOCKSETVERSION  [major minor patch] from simulink/VERSION, the one place it is set.
    file = fullfile(fileparts(fileparts(mfilename('fullpath'))), 'VERSION');
    text = strtrim(fileread(file));
    tok  = regexp(text, '^(\d+)\.(\d+)\.(\d+)$', 'tokens', 'once');
    if isempty(tok)
        error('nominal:library', '%s should hold major.minor.patch, not "%s"', file, text);
    end
    v = str2double(tok);
end

function prefix = pathFixPrefix()
%PATHFIXPREFIX  Inline MATLAB that puts the block's own folders on the MATLAB path.
%
%   Dragging a block into a model does not add the folder it came from to the path. Two
%   things then break, and they break in different places:
%
%     the mask callbacks   "Undefined function 'nominal_sink_detect'"
%     the S-function       "S-Function 'nominal_sink_sfun' does not exist"
%
%   The second only appears when the model is *run*, which is why fixing the callbacks
%   alone was not enough. This runs as the mask Initialization as well as ahead of each
%   callback, so it happens whenever the model compiles, however the block got there.
%
%   The block links to nominal_lib, so ReferenceBlock names the library and Simulink
%   knows the file it came from. That file sits in the blockset's library/ folder, with
%   the callbacks in script/ and the compiled S-function in mex/ beside it.
%
%   An installed toolbox puts all three on the path itself, so there this does nothing.
%   It matters when running from a clone.
%
%   Written inline rather than as a helper function because a helper would have exactly
%   the problem it is solving: it would not be on the path either.
%
%   The `which` guards make this a no-op once resolved, which matters because mask
%   Initialization runs on every compile.
%
%   nominal_setup covers what this cannot, such as a model opened where the library was
%   never loaded.

    prefix = [ ...
        'if isempty(which(''nominal_sink_detect'')) || isempty(which(''nominal_sink_sfun'')), ' ...
            'nm_rb = get_param(gcb, ''ReferenceBlock''); ' ...
            'if ~isempty(nm_rb), ' ...
                'nm_lib = strtok(nm_rb, ''/''); ' ...
                'if bdIsLoaded(nm_lib), ' ...
                    'nm_d = fileparts(fileparts(get_param(nm_lib, ''FileName''))); ' ...
                    'if ~isempty(nm_d), ' ...
                        'nm_s = fullfile(nm_d, ''script''); ' ...
                        'if isempty(which(''nominal_sink_detect'')) && isfolder(nm_s), addpath(nm_s); end, ' ...
                        'nm_m = fullfile(nm_d, ''mex''); ' ...
                        'if isempty(which(''nominal_sink_sfun'')) && isfolder(nm_m), addpath(nm_m); end, ' ...
                    'end, ' ...
                'end, ' ...
            'end, ' ...
        'end, '];
end
