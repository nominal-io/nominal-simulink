function names = nominal_sink_autonames(blk, apply)
%NOMINAL_SINK_AUTONAMES  Work out channel names from whatever is wired into the block.
%
%   names = NOMINAL_SINK_AUTONAMES(blk) returns a cellstr of channel names inferred
%   from the signal connected to the block's input.
%
%   NOMINAL_SINK_AUTONAMES(blk, true) also writes them into the block's `channels`
%   parameter. This is what the "Detect from connected signal" button does.
%
%   Typing channel names by hand is both tedious and a silent source of error: the list
%   sets the input port width, so a name added without a matching signal — or a signal
%   added without a name — misaligns every column after it. Reading them off the model
%   removes both problems.
%
%   Handles the shapes people actually build:
%
%     Bus Creator   element names, which is the case worth optimising for — adding a
%                   signal to the bus then means editing the bus, not the block.
%     Mux           the name of each incoming line, or its source block if the line is
%                   unnamed.
%     Bus Selector  the selected signal names.
%     anything else the line's own name, or the source block's name.
%
%   Deliberately does not compile the model. A button in a dialog should respond
%   immediately, and compiling to resolve widths would make it slow and would fail
%   outright on a model that is not yet complete — which is exactly when someone is
%   wiring things up and wants this.
%
%   The consequence is that a *vector* signal of width N carrying one name yields one
%   name, not N. There is no way to know the width without compiling. The block will
%   then report a width mismatch, which is a clear error rather than silent
%   misalignment, and the fix is to name the elements or use a Mux.
%
%   See also NOMINAL_SINK_MODECB

    if nargin < 2
        apply = false;
    end

    ph = get_param(blk, 'PortHandles');
    if isempty(ph.Inport)
        error('nominal:autonames', 'the block has no input port');
    end

    line = get_param(ph.Inport(1), 'Line');
    if line <= 0
        error('nominal:autonames', ...
              ['nothing is connected to the block''s input. Wire up the signals ' ...
               'first, then detect.']);
    end

    names = namesFromLine(line, 0);

    if isempty(names)
        error('nominal:autonames', ...
              'could not work out any channel names from the connected signal');
    end

    names = makeUnique(names);

    if apply
        % The parameter holds a MATLAB expression, so the value is a quoted literal.
        set_param(blk, 'channels', ['''' strjoin(names, ', ') '''']);
    end
end

% ------------------------------------------------------------------ walking

function names = namesFromLine(line, depth)
    names = {};
    if depth > 8
        % Guards against a pathological or cyclic diagram rather than any real model.
        return;
    end

    srcPort = get_param(line, 'SrcPortHandle');
    if srcPort <= 0
        return;
    end
    srcBlk = get_param(srcPort, 'Parent');

    switch get_param(srcBlk, 'BlockType')
        case 'BusCreator'
            % The case worth optimising for: names are declared on the bus, so adding a
            % signal later means editing the bus rather than the block.
            names = cellstr(get_param(srcBlk, 'InputSignalNames'));
            names = names(:).';

        case 'Mux'
            % Each input contributes one channel. Recursing rather than reading names
            % directly means a Bus Creator feeding a Mux still works.
            mph = get_param(srcBlk, 'PortHandles');
            for i = 1:numel(mph.Inport)
                l = get_param(mph.Inport(i), 'Line');
                if l > 0
                    names = [names, namesFromLine(l, depth + 1)]; %#ok<AGROW>
                else
                    names{end+1} = sprintf('in%d', i); %#ok<AGROW>
                end
            end

        case 'BusSelector'
            sel = strtrim(strsplit(get_param(srcBlk, 'OutputSignals'), ','));
            if strcmp(get_param(srcBlk, 'OutputAsBus'), 'on')
                names = sel;
            else
                % Each selected signal leaves on a port of its own, so this line carries
                % only the one on the port it is wired to. Naming them all would set the
                % input width to the whole selection and fail at compile.
                names = sel(get_param(srcPort, 'PortNumber'));
            end

        otherwise
            names = {signalName(line, srcBlk)};
    end

    names = names(~cellfun(@isempty, names));
end

function n = signalName(line, srcBlk)
% The line's own name if it has one, else the source block's. A named line is the
% user's explicit intent and beats a block name like "Sine Wave1".
    n = get_param(line, 'Name');
    if isempty(n)
        n = get_param(srcBlk, 'Name');
    end
    n = strtrim(strrep(n, newline, ' '));
end

function names = makeUnique(names)
% Duplicate names would collapse two signals into one channel in Nominal, losing data
% silently. Suffix instead, and leave it visible so the user can rename properly.
%
% A suffix must avoid every name in the list, not only the ones already seen: with
% {'x', 'x', 'x_2'} the second 'x' cannot become 'x_2', or the third signal's own name
% would then collide with it.
    original = names;
    used = containers.Map('KeyType', 'char', 'ValueType', 'logical');
    for i = 1:numel(names)
        base = names{i};
        name = base;
        k = 1;
        while used.isKey(name) || (k > 1 && any(strcmp(original, name)))
            k = k + 1;
            name = sprintf('%s_%d', base, k);
        end
        names{i} = name;
        used(name) = true;
    end
end
