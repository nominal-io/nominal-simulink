function stats = nmproxy_reference(varargin)
%NMPROXY_REFERENCE  A readable proxy: receive frames, decode them, report what arrived.
%
%   NMPROXY_REFERENCE() listens on 127.0.0.1:15400 until you press Ctrl-C.
%   NMPROXY_REFERENCE('Port', 15400, 'Seconds', 30, 'Verbose', true)
%   stats = NMPROXY_REFERENCE(...) returns counts when it stops.
%
%   This exists to be read. The production relay is nmproxy (Go): it writes to Nominal,
%   handles many concurrent sessions, and runs as a service. This one writes nothing —
%   it decodes frames and prints what it sees, which is enough to answer "is the model
%   actually sending, and does the data look right?" and enough to show anyone
%   implementing a proxy in another language what the job involves.
%
%   Uses java.net.DatagramSocket rather than udpport, so it needs no Instrument Control
%   Toolbox — stock MATLAB is enough.
%
%   To turn it into something that writes, replace the marked section with a call to
%   whichever client you use. Everything before that point is the protocol.
%
%   See also NMFRAME_DECODE, NMFRAME_CONFIGID

    opts = parseArgs(varargin{:});

    sock = java.net.DatagramSocket(opts.Port);
    % A receive timeout is what makes Ctrl-C work: a blocking receive would ignore it.
    sock.setSoTimeout(200);
    closer = onCleanup(@() sock.close()); %#ok<NASGU>

    fprintf('listening on udp/%d — Ctrl-C to stop\n', opts.Port);

    buf = zeros(1, 2048, 'int8');
    pkt = java.net.DatagramPacket(buf, numel(buf));

    configs  = containers.Map('KeyType', 'char', 'ValueType', 'any');
    sessions = containers.Map('KeyType', 'char', 'ValueType', 'any');
    stats    = struct('rows', 0, 'gaps', 0, 'reorders', 0, ...
                      'unknownConfig', 0, 'badFrames', 0);

    started  = tic;
    lastLog  = tic;

    while true
        if opts.Seconds > 0 && toc(started) >= opts.Seconds
            break;
        end

        pkt.setLength(numel(buf));
        try
            sock.receive(pkt);
        catch
            % Timed out with nothing to read. Not an error — it is how the loop stays
            % interruptible.
            continue;
        end

        n     = pkt.getLength();
        raw   = typecast(pkt.getData(), 'uint8');
        bytes = raw(1:n);

        try
            frame = nmframe_decode(bytes);
        catch err
            % Anything that can reach this port can send rubbish, so a bad datagram is
            % a counter, never a reason to stop.
            stats.badFrames = stats.badFrames + 1;
            if stats.badFrames == 1
                fprintf('rejected a frame: %s\n', err.message);
            end
            continue;
        end

        % Hex, not sprintf('%d', ...). MATLAB renders a uint64 above about 2^53 in
        % scientific notation under %d, which both looks wrong and — because these
        % strings are map keys — would let two distinct ids collide on one entry.
        key = idKey(frame.configId);

        switch frame.type
            case 'hello'
                if ~configs.isKey(key)
                    % Optional but worth doing once: if the sender's hash disagrees with
                    % ours, every DATA frame would be discarded as an unknown
                    % configuration and nothing would ever explain why.
                    if ~isequal(nmframe_configid(frame.datasetRid, frame.channels), frame.configId)
                        fprintf(['HELLO config_id does not match its contents — ' ...
                                 'the sender''s hash disagrees with ours\n']);
                        stats.badFrames = stats.badFrames + 1;
                        continue;
                    end
                    configs(key) = frame; %#ok<NASGU>
                    fprintf('configuration registered: %s, %d channels (%s)\n', ...
                            frame.datasetRid, numel(frame.channels), ...
                            strjoin(frame.channels, ', '));
                end

            case 'data'
                if ~configs.isKey(key)
                    % Normal after a restart, and self-correcting: the host repeats
                    % HELLO about once a second.
                    stats.unknownConfig = stats.unknownConfig + 1;
                    continue;
                end
                cfg = configs(key);

                skey = idKey(frame.sessionId);
                if sessions.isKey(skey)
                    s = sessions(skey);
                else
                    s = struct('nextSeq', frame.sequence, 'rows', 0);
                    fprintf('session started: %s\n', skey);
                end

                % Sequence numbers are the only visibility into loss, since a dropped
                % datagram leaves no other trace.
                if frame.sequence > s.nextSeq
                    stats.gaps = stats.gaps + double(frame.sequence - s.nextSeq);
                    s.nextSeq = frame.sequence + 1;
                elseif frame.sequence < s.nextSeq
                    % UDP may reorder. The frame was already counted as a gap when its
                    % successor arrived, so counting it again would double-count.
                    stats.reorders = stats.reorders + 1;
                else
                    s.nextSeq = frame.sequence + 1;
                end

                s.rows      = s.rows + size(frame.values, 1);
                sessions(skey) = s; %#ok<NASGU>
                stats.rows  = stats.rows + size(frame.values, 1);

                % --- where a real proxy would write -----------------------------
                %
                % cfg.channels are the channel names in order; frame.values has one
                % column per channel and one row per sample; frame.timestamps holds the
                % matching nanoseconds since the epoch. Hand those to whichever client
                % you use. nmproxy does exactly this against nominal-streaming-go.
                if opts.Verbose
                    fprintf('  seq %6d  %d rows  first: t=%d  [%s]\n', ...
                            frame.sequence, size(frame.values, 1), ...
                            frame.timestamps(1), ...
                            strjoin(compose('%g', frame.values(1, :)), ', '));
                end
                % ----------------------------------------------------------------
        end

        if toc(lastLog) >= 5
            printStats(stats, configs.Count, sessions.Count);
            lastLog = tic;
        end
    end

    printStats(stats, configs.Count, sessions.Count);
end

% ------------------------------------------------------------------ helpers

function s = idKey(v)
%IDKEY  A 64-bit id as an exact hex string, for map keys and for printing.
    s = lower(dec2hex(v, 16));
end

function opts = parseArgs(varargin)
    p = inputParser();
    p.addParameter('Port', 15400, @(x) isnumeric(x) && x > 0 && x < 65536);
    p.addParameter('Seconds', 0, @isnumeric);   % 0 means until interrupted
    p.addParameter('Verbose', false, @islogical);
    p.parse(varargin{:});
    opts = p.Results;
end

function printStats(stats, nConfigs, nSessions)
    fprintf('configs=%d sessions=%d rows=%d gaps=%d reorders=%d unknown_config=%d bad_frames=%d\n', ...
            nConfigs, nSessions, stats.rows, stats.gaps, stats.reorders, ...
            stats.unknownConfig, stats.badFrames);
end
