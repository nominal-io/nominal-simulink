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

    blk = [model '/sink'];
    add_block('built-in/S-Function', blk, ...
        'FunctionName', 'nominal_sink_sfun', ...
        'Parameters', 'channels, datasetRid, baseUrl, sampleTime, capacityRows, timeMode');

    % Three channels, so the input port should size itself to 3.
    set_param(blk, ...
        'channels',     '''smoke.a, smoke.b, smoke.c''', ...
        'datasetRid',   '''ri.catalog.test.dataset.0''', ...
        'baseUrl',      '''https://127.0.0.1:1''', ...
        'sampleTime',   '0.01', ...
        'capacityRows', '64', ...
        'timeMode',     '1');

    src = [model '/src'];
    add_block('simulink/Sources/Constant', src, 'Value', '[1 2 3]');
    add_line(model, 'src/1', 'sink/1');

    term = [model '/term'];
    add_block('simulink/Sinks/Terminator', term);
    add_line(model, 'sink/1', 'term/1');

    set_param(model, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                     'FixedStep', '0.01');

    % 3 --------------------------------------------------- port width follows channels
    try
        c = get_param(blk, 'CompiledPortWidths');
        failures = report(failures, isequal(c.Inport, 3), ...
                          'input port width follows the channel list');
    catch
        % CompiledPortWidths needs a compiled model; fall through to the run below,
        % which is the stronger check anyway.
        fprintf('note: port widths unavailable before compile\n');
    end

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
