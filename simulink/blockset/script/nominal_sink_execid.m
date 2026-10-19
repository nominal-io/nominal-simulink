function id = nominal_sink_execid(executionId, tags)
%NOMINAL_SINK_EXECID  Turn the block's Execution ID field into the text the S-function reads.
%
%   The block sends the ID as the execution-id tag on every channel. 'auto' passes
%   through, and the S-function makes a new ID for each run when it starts: made here, an
%   ID would be fixed into a model built for an accelerated mode and repeat across runs.
%   Text or a number is used as it is, so a script can set each run's ID through a
%   variable. Empty sends no ID.
%
%   tags is the Tags field, checked so that it doesn't set execution-id as well.
%
%   The block's Parameters call this, so each run reads the current value of whatever
%   variable the field names.

    if isempty(executionId)
        id = '';
        return;
    end
    if ~((ischar(executionId) && isrow(executionId)) || ...
         ((isstring(executionId) || isnumeric(executionId) || islogical(executionId)) && ...
          isscalar(executionId))) || ismissing(string(executionId))
        error('nominal:execid', ...
              ['Nominal Stream Sink: Execution ID must be one text value or number, such ' ...
               'as ''run-7'' or 7.']);
    end
    id = char(string(executionId));
    if any(id < 32)
        error('nominal:execid', ...
              'Nominal Stream Sink: Execution ID contains a line break or another control character.');
    end

    % nominal_sink_tags joins each key to its value with char(31), and tags with char(30).
    pairs = strsplit(nominal_sink_tags(tags), char(30));
    if any(startsWith(pairs, ['execution-id' char(31)]))
        error('nominal:execid', ...
              ['Nominal Stream Sink: Tags has an execution-id key, which Execution ID also ' ...
               'sets. Remove one of them.']);
    end
end
