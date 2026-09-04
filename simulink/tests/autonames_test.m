function autonames_test()
%AUTONAMES_TEST  Channel-name detection across the shapes people actually wire up.
%
%   The button itself cannot be clicked in -batch, so this exercises the function behind
%   it. Needs no token and no network.

    fprintf('--- channel name detection ---\n');
    failures = 0;

    if ~bdIsLoaded('nominal_lib')
        load_system('nominal_lib');
    end

    failures = check(failures, 'Mux of named lines', ...
        @() buildMux(true), {'alpha', 'beta', 'gamma'});

    % Unnamed lines fall back to the source block names, which is worse than naming the
    % signals but far better than failing.
    failures = check(failures, 'Mux of unnamed lines uses block names', ...
        @() buildMux(false), {'s1', 's2', 's3'});

    failures = check(failures, 'Bus Creator uses its element names', ...
        @buildBus, {'rpm', 'egt', 'psi'});

    % Two signals sharing a name would collapse into one channel in Nominal and lose
    % data with no error, so they must be made distinct.
    failures = check(failures, 'duplicate names are made unique', ...
        @buildDuplicates, {'dup', 'dup_2'});

    failures = failures + expectError('an unconnected input is reported', @buildUnconnected);

    fprintf('--- %s ---\n', pick(failures == 0, 'all passed', 'FAILURES'));
    if failures > 0
        error('autonames_test:failed', '%d check(s) failed', failures);
    end
end

% ------------------------------------------------------------------ cases

function [model, blk] = buildMux(named)
    [model, blk] = newModel('nm_autonames_mux');
    add_block('simulink/Signal Routing/Mux', [model '/mux'], 'Inputs', '3');
    labels = {'alpha', 'beta', 'gamma'};
    for i = 1:3
        src = sprintf('%s/s%d', model, i);
        add_block('simulink/Sources/Constant', src, 'Value', num2str(i));
        h = add_line(model, sprintf('s%d/1', i), sprintf('mux/%d', i));
        if named
            set_param(h, 'Name', labels{i});
        end
    end
    add_line(model, 'mux/1', 'sink/1');
end

function [model, blk] = buildBus()
    [model, blk] = newModel('nm_autonames_bus');
    add_block('simulink/Signal Routing/Bus Creator', [model '/bus'], 'Inputs', '3');
    labels = {'rpm', 'egt', 'psi'};
    for i = 1:3
        src = sprintf('%s/s%d', model, i);
        add_block('simulink/Sources/Constant', src, 'Value', num2str(i));
        h = add_line(model, sprintf('s%d/1', i), sprintf('bus/%d', i));
        set_param(h, 'Name', labels{i});
    end
    add_line(model, 'bus/1', 'sink/1');
end

function [model, blk] = buildDuplicates()
    [model, blk] = newModel('nm_autonames_dup');
    add_block('simulink/Signal Routing/Mux', [model '/mux'], 'Inputs', '2');
    for i = 1:2
        src = sprintf('%s/s%d', model, i);
        add_block('simulink/Sources/Constant', src, 'Value', num2str(i));
        h = add_line(model, sprintf('s%d/1', i), sprintf('mux/%d', i));
        set_param(h, 'Name', 'dup');
    end
    add_line(model, 'mux/1', 'sink/1');
end

function [model, blk] = buildUnconnected()
    [model, blk] = newModel('nm_autonames_none');
end

% ------------------------------------------------------------------ harness

function [model, blk] = newModel(name)
    if bdIsLoaded(name)
        close_system(name, 0);
    end
    new_system(name);
    load_system(name);
    blk = [name '/sink'];
    add_block('nominal_lib/Nominal Stream Sink', blk);
    model = name;
end

function n = check(n, what, builder, expected)
    model = '';
    try
        [model, blk] = builder();
        got = nominal_sink_autonames(blk, false);
        if isequal(got, expected)
            fprintf('ok:   %s\n', what);
        else
            fprintf('FAIL: %s\n      got %s, want %s\n', what, ...
                    strjoin(got, '|'), strjoin(expected, '|'));
            n = n + 1;
        end
    catch err
        fprintf('FAIL: %s\n      %s\n', what, err.message);
        n = n + 1;
    end
    closeQuietly(model);
end

function n = expectError(what, builder)
    n = 0;
    model = '';
    try
        [model, blk] = builder();
        nominal_sink_autonames(blk, false);
        fprintf('FAIL: %s (no error raised)\n', what);
        n = 1;
    catch
        fprintf('ok:   %s\n', what);
    end
    closeQuietly(model);
end

function closeQuietly(model)
    if ~isempty(model) && bdIsLoaded(model)
        close_system(model, 0);
    end
end

function s = pick(cond, a, b)
    if cond
        s = a;
    else
        s = b;
    end
end
