function make_docs()
%MAKE_DOCS  Generate the blockset's HTML documentation from simulink/doc-src.
%
%   Each page is a script of publish markup, so the source is plain text a diff can
%   review. This publishes them into blockset/doc/html, writes the help browser's table
%   of contents beside them, and builds a search index. blockset/doc/info.xml points the
%   help browser at the result, and the help browser lists the pages under Supplemental
%   Software.
%
%   Run it under both the oldest and the newest supported release, as `just docs` does.
%   The search index format changed in R2022a, so each release builds the index it
%   reads, and both ship.
%
%   The page names matter. nominal_help finds a block's page from its mask type, so the
%   page for 'Nominal Stream Sink' must be nominal_stream_sink.html.

    simulinkDir = fileparts(fileparts(mfilename('fullpath')));
    srcDir  = fullfile(simulinkDir, 'doc-src');
    htmlDir = fullfile(simulinkDir, 'blockset', 'doc', 'html');
    version = strtrim(fileread(fullfile(simulinkDir, 'VERSION')));

    % In table-of-contents order. The first is the landing page.
    pages = {
        'nominal_blockset',     'Nominal Blockset',          ''
        'nominal_stream_sink',  'Nominal Stream Sink',       'HelpIcon.BLOCK'
        'streaming_via_proxy',  'Streaming Through a Relay', 'HelpIcon.USER_GUIDE'
        'troubleshooting',      'Troubleshooting',           'HelpIcon.USER_GUIDE'
    };

    if ~isfolder(htmlDir)
        mkdir(htmlDir);
    end

    for i = 1:size(pages, 1)
        src = fullfile(srcDir, [pages{i, 1} '.m']);
        % evalCode off: the sources are prose, and nothing in a doc build should run.
        out = publish(src, 'format', 'html', 'outputDir', htmlDir, ...
                      'evalCode', false, 'showCode', false, 'createThumbnail', false);
        html = readText(out);
        writeText(out, strrep(html, '@VERSION@', version));
    end

    writeText(fullfile(htmlDir, 'helptoc.xml'), toc(pages));

    % builddocsearchdb refuses a folder no installed product claims. MATLAB learns of
    % info.xml, which makes the claim, when its folder is added to the path.
    addpath(fileparts(htmlDir));
    builddocsearchdb(htmlDir);

    fprintf('wrote %d pages and a search index to %s\n', size(pages, 1), htmlDir);
end

function xml = toc(pages)
    item = @(i) sprintf('<tocitem target="%s.html"%s>%s</tocitem>', pages{i, 1}, ...
                        iconAttr(pages{i, 3}), pages{i, 2});
    children = arrayfun(@(i) ['    ' item(i) newline], 2:size(pages, 1), ...
                        'UniformOutput', false);
    xml = [ ...
        '<?xml version="1.0" encoding="utf-8"?>' newline ...
        '<toc version="2.0">' newline ...
        sprintf('  <tocitem target="%s.html">%s', pages{1, 1}, pages{1, 2}) newline ...
        [children{:}] ...
        '  </tocitem>' newline ...
        '</toc>' newline];
end

function a = iconAttr(icon)
    if isempty(icon)
        a = '';
    else
        a = sprintf(' image="%s"', icon);
    end
end

function text = readText(file)
% Not fileread: R2021b's reads in the system code page and takes no encoding, which
% would turn every em dash into mojibake on the way back out.
    fid = fopen(file, 'r', 'n', 'UTF-8');
    if fid < 0
        error('nominal:docs', 'cannot read %s', file);
    end
    closer = onCleanup(@() fclose(fid));
    text = fread(fid, '*char').';
end

function writeText(file, text)
    fid = fopen(file, 'w', 'n', 'UTF-8');
    if fid < 0
        error('nominal:docs', 'cannot write %s', file);
    end
    closer = onCleanup(@() fclose(fid));
    fwrite(fid, text, 'char');
end
