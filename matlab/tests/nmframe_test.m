function nmframe_test()
%NMFRAME_TEST  Check the MATLAB decoder against the shared golden fixtures.
%
%   The fixtures in cstream/tests/fixtures/frames.txt are produced by the C encoder.
%   Nothing here shares code with it, so agreeing on these bytes is the only evidence
%   that this decoder actually interoperates — three independent implementations are
%   worth having only if something holds them together.
%
%   Needs no token and no network.

    fprintf('--- MATLAB frame decoder ---\n');
    failures = 0;

    here    = fileparts(mfilename('fullpath'));
    fixture = fullfile(here, '..', '..', 'cstream', 'tests', 'fixtures', 'frames.txt');
    fx      = readFixtures(fixture);

    % Documented contents of the fixture, written out longhand rather than derived. A
    % test that computes its expectations the way the code does will happily confirm a
    % shared mistake.
    wantRid       = 'ri.catalog.test.dataset.abc';
    wantChannels  = {'engine.rpm', 'engine.egt', 'engine.psi'};
    % Hex literals, not hex2dec: hex2dec goes via double and silently rounds anything
    % above 2^53, which would make these expectations wrong in exactly the way the test
    % exists to catch.
    wantSession   = 0x1122334455667788u64;
    wantConfig    = 0x19915455f27bd0f7u64;
    wantSequence  = uint64(42);
    wantTimes     = int64([1000000000; 1000001000; 1000002000]);
    wantValues    = [1500.00 700.0 30.0
                     1510.50 701.5 30.1
                     1521.25 703.0 30.2];

    % 1 ------------------------------------------------------------------ config_id
    %
    % The check that matters most. If this disagrees with the sender, every DATA frame
    % is rejected as an unknown configuration and no data arrives, with no error
    % anywhere to explain it.
    got = nmframe_configid(wantRid, wantChannels);
    failures = report(failures, isequal(got, wantConfig), ...
        sprintf('config_id matches the C encoder (got %s)', hexOf(got)));

    % The canonical FNV-1a vector, independent of our own fixtures. Any new
    % implementation should pass this before anything else is trusted.
    failures = report(failures, isequal(fnvOfSingleA(), 0xaf63dc4c8601ec8cu64), ...
        'FNV-1a of the single byte ''a'' matches the published vector');

    % 2 ---------------------------------------------------------------------- HELLO
    h = nmframe_decode(fx.hello);
    failures = report(failures, strcmp(h.type, 'hello'), 'HELLO decodes as a HELLO');
    failures = report(failures, isequal(h.sessionId, wantSession), 'HELLO carries the session id');
    failures = report(failures, isequal(h.configId, wantConfig), 'HELLO carries the config id');
    failures = report(failures, strcmp(h.datasetRid, wantRid), 'HELLO carries the dataset RID');
    failures = report(failures, isequal(h.channels, wantChannels), 'HELLO carries the channel names in order');

    % The mapping only works if the announced channels hash to the announced config.
    failures = report(failures, ...
        isequal(nmframe_configid(h.datasetRid, h.channels), h.configId), ...
        'HELLO is internally consistent');

    % 3 ----------------------------------------------------------------------- DATA
    d = nmframe_decode(fx.data);
    failures = report(failures, strcmp(d.type, 'data'), 'DATA decodes as a DATA');
    failures = report(failures, isequal(d.sequence, wantSequence), 'DATA carries the sequence number');
    failures = report(failures, isequal(d.timestamps, wantTimes), 'timestamps survive the round trip');
    % Exact comparison, not a tolerance: the bytes should round-trip bit for bit, and a
    % tolerance would hide a real encoding bug in the low mantissa.
    failures = report(failures, isequal(d.values, wantValues), 'values survive the round trip exactly');

    % 4 ------------------------------------------------------------------ malformed
    %
    % These datagrams arrive unauthenticated, so anything that can reach the port can
    % send whatever it likes. Every malformed input must raise rather than return
    % something plausible.
    failures = report(failures, allTruncationsRejected(fx.hello), ...
        'HELLO truncated at any length is rejected');
    failures = report(failures, allTruncationsRejected(fx.data), ...
        'DATA truncated at any length is rejected');

    failures = report(failures, rejects(@() nmframe_decode(corrupt(fx.hello, 1, 255))), ...
        'bad magic is rejected');
    failures = report(failures, rejects(@() nmframe_decode(corrupt(fx.hello, 5, 99))), ...
        'an unsupported version is rejected');
    failures = report(failures, rejects(@() nmframe_decode(corrupt(fx.hello, 6, 77))), ...
        'an unknown frame type is rejected');

    % A full-length frame whose internal counts claim more than it carries — the case
    % truncation cannot cover. row_count sits after the header, sequence and
    % channel_count.
    inflated = fx.data;
    inflated(24 + 11) = 255;
    inflated(24 + 12) = 255;
    failures = report(failures, rejects(@() nmframe_decode(inflated)), ...
        'an inflated row count is rejected');

    fprintf('--- %s ---\n', pickString(failures == 0, 'all passed', 'FAILURES'));
    if failures > 0
        error('nmframe_test:failed', '%d check(s) failed', failures);
    end
end

% ------------------------------------------------------------------ helpers

function fx = readFixtures(path)
    fx = struct();
    fid = fopen(path, 'r');
    if fid < 0
        error('nmframe_test:fixture', 'cannot open %s', path);
    end
    cleanup = onCleanup(@() fclose(fid)); %#ok<NASGU>

    while true
        line = fgetl(fid);
        if ~ischar(line)
            break;
        end
        line = strtrim(line);
        if isempty(line) || line(1) == '#'
            continue;
        end
        parts = strsplit(line);
        if numel(parts) ~= 2
            continue;
        end
        key = parts{1};
        val = parts{2};
        if all(isstrprop(val, 'xdigit')) && mod(numel(val), 2) == 0 && numel(val) > 16
            fx.(key) = uint8(sscanf(val, '%2x')');
        end
    end
end

function tf = allTruncationsRejected(bytes)
    tf = true;
    for n = 0:numel(bytes)-1
        if ~rejects(@() nmframe_decode(bytes(1:n)))
            fprintf('  truncating to %d bytes was accepted\n', n);
            tf = false;
        end
    end
end

function tf = rejects(fn)
    try
        fn();
        tf = false;
    catch
        tf = true;
    end
end

function b = corrupt(bytes, index, value)
    b = bytes;
    b(index) = uint8(value);
end

function h = fnvOfSingleA()
% The published vector hashes the byte 'a' alone, with no length prefix, so it cannot go
% through nmframe_configid. Reproduced here so a new implementation has something to
% check before trusting anything else.
    h = bitxor(0xcbf29ce484222325u64, uint64(uint8('a')));
    h = nmframe_mulmod64(h, 0x100000001b3u64);
end

function s = hexOf(v)
    s = sprintf('0x%016s', lower(dec2hex(v, 16)));
end

function n = report(n, cond, what)
    if cond
        fprintf('ok:   %s\n', what);
    else
        fprintf('FAIL: %s\n', what);
        n = n + 1;
    end
end

function s = pickString(cond, a, b)
    if cond
        s = a;
    else
        s = b;
    end
end
