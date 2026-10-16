function file = nominal_help(blk)
%NOMINAL_HELP  Open the Nominal blockset documentation.
%
%   NOMINAL_HELP             the blockset's landing page
%   NOMINAL_HELP(blk)        the page for the block at path or handle blk
%   NOMINAL_HELP(blockType)  the page for a block type, e.g. 'Nominal Stream Sink'
%   file = NOMINAL_HELP(...) the page's file, without opening it
%
%   A block's Help button calls this with gcb. The page is looked up when the button is
%   pressed rather than written into the block when the library is built, because the
%   blockset's folder is not known then.

    page = 'nominal_blockset.html';

    if nargin > 0 && ~isempty(blk)
        try
            type = get_param(blk, 'MaskType');
        catch
            type = blk;   % not a block, so take it as a block type
        end
        candidate = [regexprep(lower(strtrim(type)), '[^a-z0-9]+', '_') '.html'];
        if isfile(fullfile(nominal_root(), 'doc', 'html', candidate))
            page = candidate;
        end
    end

    path = fullfile(nominal_root(), 'doc', 'html', page);
    if ~isfile(path)
        error('nominal:help', ...
              ['%s is missing. In a clone, generate the documentation with ' ...
               '`just docs`. In an installed toolbox, reinstall the Nominal Blockset.'], ...
              path);
    end

    if nargout > 0
        file = path;
    else
        web(path);
    end
end
