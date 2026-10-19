function frame = nmframe_decode(bytes)
%NMFRAME_DECODE  Decode one host-to-proxy datagram.
%
%   frame = NMFRAME_DECODE(bytes) takes a uint8 row vector — the payload of one UDP
%   datagram — and returns a struct describing it.
%
%   Common to every frame type:
%       .type        'hello', 'data' or 'end'
%       .version     protocol version byte
%       .sessionId   uint64, identifies one execution of a model
%       .configId    uint64, identifies a dataset, channel list and tags
%
%   HELLO adds:
%       .datasetRid  char
%       .channels    cellstr, in order
%       .tags        N-by-2 cell array of char, one {key, value} row per tag, in order.
%                    Every channel carries them.
%       .run         the run to make for the session, a struct with
%                      .mode      'none', 'own' for a run of its own, or 'shared' for
%                                 one run for every session with this title
%                      .start     int64, nanoseconds since the Unix epoch
%                      .tagCount  the run shows the points with the first this-many tags
%                      .title     char
%                      .refName   char, the name the dataset goes under in the run
%
%   DATA adds:
%       .sequence    uint64, increases by one per DATA frame within a session. A
%                    skipped value means a frame was lost or is arriving late.
%       .timestamps  int64 column, nanoseconds since the Unix epoch
%       .values      double matrix, one row per sample, one column per channel
%
%   END adds:
%       .endTime     int64, nanoseconds since the Unix epoch: the newest row's timestamp.
%                    A sender sends END several times, so act on the first.
%
%   Throws if the datagram is malformed. Callers receiving from a network should catch
%   that rather than let it stop a receive loop: these bytes are unauthenticated, so
%   anything that can reach the port can send rubbish.
%
%   This is a reference implementation. It is written to be read alongside the format
%   description in cstream/src/nm_frame.h, and it is deliberately independent of the C
%   and Go implementations — the three agree only because they are all pinned to the
%   fixtures in cstream/tests/fixtures/frames.txt. The production relay is nmproxy.
%
%   See also NMFRAME_CONFIGID, NMPROXY_REFERENCE

    MAGIC   = uint8([78 77 83 49]);   % 'NMS1', in order
    VERSION = uint8(1);
    HEADER  = 24;

    bytes = uint8(bytes(:).');
    if numel(bytes) < HEADER
        error('nmframe:short', 'datagram is %d bytes, shorter than the %d-byte header', ...
              numel(bytes), HEADER);
    end
    if ~isequal(bytes(1:4), MAGIC)
        error('nmframe:magic', 'wrong magic: %s', mat2str(bytes(1:4)));
    end
    if bytes(5) ~= VERSION
        % Worth distinguishing from other malformed input. There is no back-channel to
        % tell the sender, so without saying this out loud a version mismatch is
        % indistinguishable from a network fault.
        error('nmframe:version', ...
              'frame is protocol version %d but this decoder supports version %d', ...
              bytes(5), VERSION);
    end

    frame = struct();
    frame.version   = bytes(5);
    frame.sessionId = readU64(bytes, 9);
    frame.configId  = readU64(bytes, 17);

    at = HEADER + 1;
    switch bytes(6)
        case 1
            frame.type = 'hello';
            [frame, ~] = decodeHello(frame, bytes, at);
        case 2
            frame.type = 'data';
            [frame, ~] = decodeData(frame, bytes, at);
        case 3
            frame.type = 'end';
            requireBytes(bytes, at, 8, 'the end time');
            frame.endTime = typecast(bytes(at:at+7), 'int64');
        otherwise
            error('nmframe:type', 'unknown frame type %d', bytes(6));
    end
end

% ------------------------------------------------------------------ bodies

function [frame, at] = decodeHello(frame, bytes, at)
    [frame.datasetRid, at] = readString(bytes, at);

    [count, at] = readU16(bytes, at);
    if count == 0
        error('nmframe:malformed', 'HELLO declares no channels');
    end

    frame.channels = cell(1, count);
    for i = 1:count
        [frame.channels{i}, at] = readString(bytes, at);
    end

    [count, at] = readU16(bytes, at);
    if count > 64
        error('nmframe:malformed', 'HELLO declares %d tags, more than the 64 allowed', count);
    end

    frame.tags = cell(count, 2);
    for i = 1:count
        [frame.tags{i, 1}, at] = readString(bytes, at);
        if isempty(frame.tags{i, 1})
            error('nmframe:malformed', 'HELLO tag %d has an empty key', i);
        end
        [frame.tags{i, 2}, at] = readString(bytes, at);
    end

    MODES = {'none', 'own', 'shared'};
    requireBytes(bytes, at, 11, 'the run');
    mode = bytes(at);
    if mode > 2
        error('nmframe:malformed', 'HELLO asks for run mode %d', mode);
    end
    run.mode     = MODES{mode + 1};
    run.start    = typecast(bytes(at+1:at+8), 'int64');
    [run.tagCount, at] = readU16(bytes, at + 9);
    [run.title, at]    = readString(bytes, at);
    [run.refName, at]  = readString(bytes, at);
    if run.tagCount > count
        error('nmframe:malformed', 'HELLO''s run shows %d tags of %d', run.tagCount, count);
    end
    if mode ~= 0 && (isempty(run.title) || isempty(run.refName))
        error('nmframe:malformed', 'HELLO asks for a run with no title or ref name');
    end
    frame.run = run;
end

function [frame, at] = decodeData(frame, bytes, at)
    frame.sequence = readU64(bytes, at);
    at = at + 8;
    [nch, at]  = readU16(bytes, at);
    [rows, at] = readU16(bytes, at);

    if nch == 0 || rows == 0
        error('nmframe:malformed', 'DATA declares %d channels and %d rows', nch, rows);
    end

    % Check the whole body is present before reading or allocating it, so an inflated
    % count raises a clear error rather than an index error or a huge zeros() allocation.
    rowBytes = 8 + 8 * double(nch);
    need = double(rows) * rowBytes;
    if at + need - 1 > numel(bytes)
        error('nmframe:malformed', ...
              'DATA claims %d rows of %d channels (%d bytes) but only %d remain', ...
              rows, nch, need, numel(bytes) - at + 1);
    end

    frame.timestamps = zeros(rows, 1, 'int64');
    frame.values     = zeros(rows, nch);

    for r = 1:rows
        p = at + (r - 1) * rowBytes;
        frame.timestamps(r) = typecast(bytes(p:p+7), 'int64');
        for c = 1:nch
            q = p + 8 * c;
            frame.values(r, c) = typecast(bytes(q:q+7), 'double');
        end
    end
end

% ------------------------------------------------------------------ readers
%
% Every read is bounds-checked against what actually arrived. typecast reinterprets
% bytes in the host's byte order, and the format is little-endian. Every platform MATLAB
% runs on is little-endian, so these agree. A big-endian host would need the bytes
% reversed first.

function [v, at] = readU16(bytes, at)
    requireBytes(bytes, at, 2, 'uint16');
    v = double(typecast(bytes(at:at+1), 'uint16'));
    at = at + 2;
end

function v = readU64(bytes, at)
    requireBytes(bytes, at, 8, 'uint64');
    v = typecast(bytes(at:at+7), 'uint64');
end

function [s, at] = readString(bytes, at)
    [n, at] = readU16(bytes, at);
    requireBytes(bytes, at, n, 'string body');
    s = char(bytes(at:at+n-1));
    at = at + n;
end

function requireBytes(bytes, at, n, what)
    if at + n - 1 > numel(bytes)
        error('nmframe:malformed', ...
              'need %d bytes for %s at offset %d, but the datagram is %d bytes', ...
              n, what, at - 1, numel(bytes));
    end
end
