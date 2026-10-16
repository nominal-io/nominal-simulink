classdef (Abstract) BlocksetTestCase < matlab.unittest.TestCase
%BLOCKSETTESTCASE  Base for tests that exercise the blockset in this clone.
%
%   Puts the clone's library/, script/ and mex/ folders on the path for the duration of
%   the class, the way an installed toolbox would, and closes every model after each
%   test so one test's leftovers cannot decide the next one's outcome.
%
%   Also supplies a dummy NOMINAL_TOKEN. Direct mode refuses to start without a token,
%   correctly, so without one the tests would pass or fail on whether the shell happened
%   to export a credential rather than on the block. Nothing here reaches a real server,
%   so the value is irrelevant.

    methods (TestClassSetup)
        function useClone(tc)
            for f = {'library', 'script', 'mex'}
                tc.applyFixture(matlab.unittest.fixtures.PathFixture( ...
                    BlocksetTestCase.folder(f{1})));
            end
        end

        function dummyToken(tc)
            BlocksetTestCase.setToken(tc, 'test-not-a-real-token');
        end
    end

    methods (TestMethodTeardown)
        function closeModels(~)
            bdclose('all');
        end
    end

    methods (Static)
        function setToken(tc, value)
        %SETTOKEN  Set NOMINAL_TOKEN until the calling test or class finishes.
        %
        %   By hand rather than with EnvironmentVariableFixture, which R2021b lacks.
            old = getenv('NOMINAL_TOKEN');
            setenv('NOMINAL_TOKEN', value);
            tc.addTeardown(@setenv, 'NOMINAL_TOKEN', old);
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
            if ~isempty(varargin)
                set_param(blk, varargin{:});
            end
            set_param(model, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                             'FixedStep', '0.01');
        end
    end
end
