// Jenkins pipeline for SystemC_Accelerator_Model.
//
// Stages: clean old reports -> configure and build (Release, -Wall -Wextra -Wpedantic) -> GoogleTest with
// JUnit output -> an ASan + UBSan build and test run -> agreement with the current SimPy
// model on freshly exported cases -> a wall-clock gate (AT and LT on four bootstraps)
// against ci/perf_baseline.json -> results.md -> an optional nightly LT quantum sweep.
//
// Needs on the agent: a SystemC 3.x install (SYSTEMC_HOME), CMake, a C++17 compiler,
// Python 3.10+ (for the export tool). Plugins: Pipeline, Git, JUnit.

pipeline {
    agent any

    parameters {
        booleanParam(name: 'NIGHTLY', defaultValue: false, description: 'Also sweep the LT global quantum')
        string(name: 'SYSTEMC_HOME', defaultValue: '', description: 'SystemC install (empty: ~/.local/opt/systemc)')
        string(name: 'PERF_MARGIN', defaultValue: '0.5', description: 'Allowed slow-down against the baseline')
    }

    options {
        buildDiscarder(logRotator(numToKeepStr: '30'))
        timeout(time: 60, unit: 'MINUTES')
    }

    environment {
        // HOME is only known to the shell; every step resolves the install the same way
        SC = "${params.SYSTEMC_HOME ?: ''}"
        SCENV = 'export SYSTEMC_HOME="${SC:-$HOME/.local/opt/systemc}"; export LD_LIBRARY_PATH="$SYSTEMC_HOME/lib"; '
    }

    stages {
        // The workspace is reused between builds (it keeps the virtualenv and build caches), so
        // delete the previous build's reports first. Without this a build that fails before its
        // tests run publishes the last build's JUnit results as its own (Rust_DES_Kernel #4 did).
        stage('Clean reports') {
            steps {
                sh 'rm -f gtest-*.xml perf_report.md sweep.csv'
            }
        }

        stage('Build') {
            steps {
                sh(env.SCENV + '''
                    python3 -m venv .venv
                    .venv/bin/pip install -q cmake "fhe-sim @ git+https://github.com/BrendanJamesLynskey/FHE_Accelerator_Sim"
                    # pip keeps an installed git dependency whose version number has not changed, so
                    # fetch FHE_Accelerator_Sim's current commit every time.
                    .venv/bin/pip install -q --force-reinstall --no-deps "fhe-sim @ git+https://github.com/BrendanJamesLynskey/FHE_Accelerator_Sim"
                    .venv/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$SYSTEMC_HOME
                    .venv/bin/cmake --build build -j2
                ''')
            }
        }

        stage('Unit and agreement tests') {
            steps {
                sh(env.SCENV + './build/accel_tests --gtest_output=xml:gtest-release.xml')
            }
        }

        stage('Sanitizers') {
            steps {
                sh(env.SCENV + '''
                    .venv/bin/cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DACCEL_SANITIZE=ON \
                        -DCMAKE_PREFIX_PATH=$SYSTEMC_HOME
                    .venv/bin/cmake --build build-asan -j2
                    ./build-asan/accel_tests --gtest_output=xml:gtest-asan.xml
                ''')
            }
        }

        stage('Agreement with the current SimPy model') {
            steps {
                sh(env.SCENV + '.venv/bin/python ci/agreement.py')
            }
        }

        stage('Performance gate') {
            steps {
                sh(env.SCENV + ".venv/bin/python ci/perf_gate.py --margin ${params.PERF_MARGIN ?: '0.5'}")
            }
            post {
                always { archiveArtifacts artifacts: 'perf_report.md', allowEmptyArchive: true }
            }
        }

        stage('Results') {
            steps {
                sh(env.SCENV + '.venv/bin/python examples/results.py > /dev/null')
                archiveArtifacts artifacts: 'examples/results.md'
            }
        }

        stage('Nightly quantum sweep') {
            when { expression { params.NIGHTLY } }
            steps {
                sh(env.SCENV + '.venv/bin/python ci/quantum_sweep.py')
                archiveArtifacts artifacts: 'sweep.csv'
            }
        }
    }

    post {
        always {
            junit testResults: 'gtest-*.xml', allowEmptyResults: true
        }
    }
}
