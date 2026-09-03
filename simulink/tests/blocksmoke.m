function blocksmoke()
%BLOCKSMOKE  Offline checks on the sink block. No token, no network.
%
%   Verifies the MEX loads, the port sizing follows the channel list, and that a model
%   containing the block actually steps. Run via `just sim-test`.
%
%   The one thing it cannot check is that data lands in Nominal — that needs a token,
%   and cstream's nmstream-demo already covers the write path end to end.

    fprintf('--- Nominal sink block smoke test ---\n');
    failures = 0;

    % 1 --------------------------------------------------------- MEX is loadable
    %
    % A statically linked MEX still fails to load if a dependency is missing, and the
    % error is unhelpful, so check this first and separately.
    if isempty(which('nominal_sink_sfun'))
        failures = report(failures, false, 'nominal_sink_sfun on the path');
    else
        failures = report(failures, true, 'nominal_sink_sfun on the path');
    end

    % 2 ----------------------------------------------------- build a model on the fly
    model = 'nm_blocksmoke_model';
    if bdIsLoaded(model)
        close_system(model, 0);
    end
    new_system(model);
    load_system(model);

    cleanup = onCleanup(@() closeQuietly(model));

    % Instantiate from the generated library rather than building a bare S-Function
    % block. The named parameters below only exist because of the mask, so this
    % exercises the thing a user actually drags in — mask and S-function together —
    % instead of a hand-wired approximation of it.
    if ~bdIsLoaded('nominal_lib')
        load_system('nominal_lib');
    end

    blk = [model '/sink'];
    add_block('nominal_lib/Nominal Stream Sink', blk);

    % Three channels, so the input port should size itself to 3.
    set_param(blk, ...
        'channels',     '''smoke.a, smoke.b, smoke.c''', ...
        'datasetRid',   '''ri.catalog.test.dataset.0''', ...
        'baseUrl',      '''https://127.0.0.1:1''', ...
        'sampleTime',   '0.01', ...
        'capacityRows', '64', ...
        ... % A popup takes its option text here, even though Evaluate is on and the
        ... % S-function receives the 1-based index. Setting '1' is rejected.
        'timeMode',     'Simulation time from start');

    src = [model '/src'];
    add_block('simulink/Sources/Constant', src, 'Value', '[1 2 3]');
    add_line(model, 'src/1', 'sink/1');

    % Deliberately nothing attached to an output, and no Terminator. Simulink treats an
    % unconnected output port as a block error, so a sink that always exposes one forces
    % every user to wire up a diagnostic they may not want — which is exactly the
    % failure this asserts is gone. showDropped defaults off, so there is no port here.
    set_param(model, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                     'FixedStep', '0.01');

    % 3 --------------------------------------------------- port width follows channels
    %
    % CompiledPortWidths only exists while the model is compiled, so compile it
    % explicitly rather than skipping the check. This is worth asserting: the port width
    % is derived from the channel list inside mdlInitializeSizes, and if the two ever
    % disagree, nm_push would read past the end of the input signal.
    widthsOk = false;
    try
        set_param(model, 'SimulationCommand', 'update');
        c = get_param(blk, 'CompiledPortWidths');
        widthsOk = isequal(c.Inport, 3) && isempty(c.Outport);
        if ~widthsOk
            fprintf('  got inport %s, outport %s\n', mat2str(c.Inport), mat2str(c.Outport));
        end
    catch err
        fprintf('  compile failed: %s\n', err.message);
    end
    % Release the compiled state whether or not the query worked, or the model stays
    % locked and the sim below fails for an unrelated reason.
    try
        set_param(model, 'SimulationCommand', 'term');
    catch
    end
    failures = report(failures, widthsOk, ...
                      'input width follows the channel list, and no output by default');

    % 4 ------------------------------------------------------------- it actually steps
    %
    % baseUrl points at a closed port, so the sender thread will fail its POSTs. That
    % is the point: a model must keep stepping when the network is unreachable, since
    % telemetry loss is never a reason to stall a simulation.
    ok = true;
    try
        sim(model, 'StopTime', '0.05');
    catch err
        ok = false;
        fprintf('  simulation error: %s\n', err.message);
    end
    failures = report(failures, ok, 'model steps with an unreachable endpoint');

    % 5 ------------------------------------------- the opt-in output port still works
    %
    % Turning it on must produce a real port, otherwise the diagnostic is unavailable
    % to the people who do want it.
    % In a fresh model rather than by mutating the one above: a model that has already
    % run a simulation does not reliably accept another compile, and the resulting
    % 'Invalid setting for SimulationCommand' says nothing about the block.
    portOk = false;
    model2 = 'nm_blocksmoke_ports';
    try
        if bdIsLoaded(model2)
            close_system(model2, 0);
        end
        new_system(model2);
        load_system(model2);
        cleanup2 = onCleanup(@() closeQuietly(model2)); %#ok<NASGU>

        blk2 = [model2 '/sink'];
        add_block('nominal_lib/Nominal Stream Sink', blk2);
        set_param(blk2, ...
            'channels',     '''smoke.a, smoke.b, smoke.c''', ...
            'datasetRid',   '''ri.catalog.test.dataset.0''', ...
            'baseUrl',      '''https://127.0.0.1:1''', ...
            'sampleTime',   '0.01', ...
            'capacityRows', '64', ...
            'timeMode',     'Simulation time from start', ...
            'showDropped',  'on');

        add_block('simulink/Sources/Constant', [model2 '/src'], 'Value', '[1 2 3]');
        add_line(model2, 'src/1', 'sink/1');
        add_block('simulink/Sinks/Terminator', [model2 '/term']);
        add_line(model2, 'sink/1', 'term/1');
        set_param(model2, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                          'FixedStep', '0.01');

        % 'Ports' is a static query — [in out enable trigger state ...] — so unlike
        % CompiledPortWidths it needs no compile. That matters here: SimulationCommand
        % is not reliably accepted once another model has simulated in the same
        % -batch session, and port *count* is all this check needs.
        p = get_param(blk2, 'Ports');
        portOk = numel(p) >= 2 && p(1) == 1 && p(2) == 1;
        if ~portOk
            fprintf('  Ports = %s, wanted 1 in and 1 out\n', mat2str(p));
        end
    catch err
        fprintf('  %s\n', err.message);
        portOk = false;
    end
    failures = report(failures, portOk, ...
                      'enabling showDropped adds a working output port');

    % 6 ------------------------------------------------------------- proxy mode runs
    %
    % Pointed at a closed port, so nothing receives the datagrams. That is the point:
    % proxy mode must not need a token, must not need a reachable relay to start, and
    % must not stall the model when the relay is absent.
    proxyOk = false;
    model3 = 'nm_blocksmoke_proxy';
    try
        if bdIsLoaded(model3)
            close_system(model3, 0);
        end
        new_system(model3);
        load_system(model3);
        cleanup3 = onCleanup(@() closeQuietly(model3)); %#ok<NASGU>

        blk3 = [model3 '/sink'];
        add_block('nominal_lib/Nominal Stream Sink', blk3);
        set_param(blk3, ...
            'channels',     '''smoke.a, smoke.b, smoke.c''', ...
            'datasetRid',   '''ri.catalog.test.dataset.0''', ...
            'mode',         'Via proxy', ...
            'proxyHost',    '''127.0.0.1''', ...
            'proxyPort',    '1', ...
            'sampleTime',   '0.01', ...
            'capacityRows', '64', ...
            'timeMode',     'Simulation time from start');

        add_block('simulink/Sources/Constant', [model3 '/src'], 'Value', '[1 2 3]');
        add_line(model3, 'src/1', 'sink/1');
        set_param(model3, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                          'FixedStep', '0.01');

        % Deliberately cleared: proxy mode must not require a credential, since the
        % whole point is that the token stays on the relay.
        saved = getenv('NOMINAL_TOKEN');
        setenv('NOMINAL_TOKEN', '');
        restore = onCleanup(@() setenv('NOMINAL_TOKEN', saved)); %#ok<NASGU>

        sim(model3, 'StopTime', '0.05');
        proxyOk = true;
    catch err
        fprintf('  %s\n', err.message);
    end
    failures = report(failures, proxyOk, ...
                      'proxy mode runs with no token and no relay listening');

    fprintf('--- %s ---\n', ternary(failures == 0, 'all passed', 'FAILURES'));
    if failures > 0
        error('blocksmoke: %d check(s) failed', failures);
    end
end

function n = report(n, cond, what)
    if cond
        fprintf('ok:   %s\n', what);
    else
        fprintf('FAIL: %s\n', what);
        n = n + 1;
    end
end

function closeQuietly(model)
    if bdIsLoaded(model)
        close_system(model, 0);
    end
end

function out = ternary(cond, a, b)
    if cond
        out = a;
    else
        out = b;
    end
end
