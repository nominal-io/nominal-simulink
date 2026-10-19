function encoded = nominal_sink_tags(tags)
%NOMINAL_SINK_TAGS  Turn the block's Tags field into the text the S-function reads.
%
%   The Tags field takes key and value pairs: a two-column string or cell array with one
%   row per tag, such as ["model" "A"; "execution-id" "7"], a dictionary (R2022b and
%   later), or a containers.Map. Empty means no tags. A value can be a number, which
%   becomes its text.
%
%   The S-function reads text parameters as one char row, so the pairs reach it joined:
%   char(31) between a key and its value, char(30) between tags. The checks below keep
%   both out of keys and values.
%
%   The block's Parameters call this, so each run reads the current value of whatever
%   variable the field names.

    keySep = char(31);
    tagSep = char(30);

    [k, v] = toPairs(tags);
    if isempty(k)
        encoded = '';
        return;
    end

    if any(k == "")
        error('nominal:tags', 'Nominal Stream Sink: a tag key is empty. Every tag needs a key.');
    end
    [uniqueKeys, first] = unique(k, 'stable');
    if numel(uniqueKeys) < numel(k)
        repeated = k(setdiff(1:numel(k), first));
        error('nominal:tags', 'Nominal Stream Sink: the tag key "%s" appears more than once.', ...
              repeated(1));
    end
    text = [k; v];
    bad  = text(contains(text, [string(keySep), string(tagSep), string(newline), string(char(13))]));
    if ~isempty(bad)
        error('nominal:tags', ...
              'Nominal Stream Sink: the tag text "%s" contains a line break or another control character.', ...
              bad(1));
    end

    encoded = char(strjoin(k + keySep + v, tagSep));
end

function [k, v] = toPairs(tags)
% Keys and values as string columns, from any of the forms the field takes.
    k = strings(0, 1);
    v = strings(0, 1);

    if isa(tags, 'dictionary')
        if numEntries(tags) > 0
            k = textOf(num2cell(keys(tags)));
            v = textOf(num2cell(values(tags)));
        end
        return;
    end
    if isa(tags, 'containers.Map')
        k = textOf(keys(tags).');
        v = textOf(values(tags).');
        return;
    end
    if isempty(tags)
        return;
    end
    if ~(isstring(tags) || iscell(tags)) || size(tags, 2) ~= 2 || ~ismatrix(tags)
        notPairs();
    end
    if isstring(tags)
        tags = num2cell(tags);
    end
    k = textOf(tags(:, 1));
    v = textOf(tags(:, 2));
end

function s = textOf(c)
% One string per cell, each from text or a number.
    s = strings(numel(c), 1);
    for i = 1:numel(c)
        x = c{i};
        if (ischar(x) && (isrow(x) || isempty(x))) || ...
           ((isstring(x) || isnumeric(x) || islogical(x)) && isscalar(x))
            s(i) = string(x);
        else
            notPairs();
        end
        if ismissing(s(i))
            notPairs();
        end
    end
end

function notPairs()
    error('nominal:tags', ...
          ['Nominal Stream Sink: Tags must be pairs of key and value, such as ' ...
           '["model" "A"; "execution-id" "7"], or a dictionary.']);
end
