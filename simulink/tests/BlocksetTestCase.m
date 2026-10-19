classdef (Abstract) BlocksetTestCase < matlab.unittest.TestCase
%BLOCKSETTESTCASE  Base for tests that exercise the blockset in this clone.
%
%   Puts the clone's library/, script/ and mex/ folders on the path for the duration of
%   the class, the way an installed toolbox would, and closes every model after each
%   test so one test's leftovers cannot decide the next one's outcome.
%
%   Also writes a profile file for the blocks to read. Direct mode refuses to start
%   without a profile, correctly, so without one the tests would pass or fail on what
%   the machine's own config file holds rather than on the block. Its URL points at a
%   closed port and its key is fake, so nothing reaches a real server.

    methods (TestClassSetup)
        function useClone(tc)
            for f = {'library', 'script', 'mex'}
                tc.applyFixture(matlab.unittest.fixtures.PathFixture( ...
                    BlocksetTestCase.folder(f{1})));
            end
        end

        function writeProfile(tc)
            file = BlocksetTestCase.profileFile();
            if ~isfolder(fileparts(file))
                mkdir(fileparts(file));
            end
            fid = fopen(file, 'w');
            fprintf(fid, ['version: 2\nprofiles:\n  default:\n' ...
                          '    base_url: https://127.0.0.1:1\n' ...
                          '    token: test-not-a-real-key\n']);
            fclose(fid);
            tc.addTeardown(@delete, file);
        end
    end

    methods (TestMethodTeardown)
        function closeModels(~)
            bdclose('all');
        end
    end

    methods (Static)
        function file = profileFile()
        %PROFILEFILE  The profile file the tests' blocks read.
            file = fullfile(tempdir, 'nominal-simulink-tests', 'config.yml');
        end

        function f = folder(name)
        %FOLDER  A folder of the blockset in this clone, e.g. folder('mex').
            f = fullfile(fileparts(fileparts(mfilename('fullpath'))), 'blockset', name);
        end

        function [model, blk] = sinkModel(name, varargin)
        %SINKMODEL  A fresh model holding the library block, unwired.
        %
        %   Instantiated from the library rather than built as a bare S-Function block:
        %   the named parameters only exist because of the mask, so this exercises what
        %   a user actually drags in. Name-value pairs set the block's parameters.
            new_system(name);
            load_system(name);
            model = name;
            blk = [name '/sink'];
            add_block('nominal_lib/Nominal Stream Sink', blk);
            % The block's default 1 ms is not a multiple of the 10 ms fixed step below,
            % and Simulink refuses the model.
            set_param(blk, 'sampleTime', '0.01', ...
                'profileFile', ['''' BlocksetTestCase.profileFile() '''']);
            if ~isempty(varargin)
                set_param(blk, varargin{:});
            end
            set_param(model, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                             'FixedStep', '0.01');
        end
    end
end
