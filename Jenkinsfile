// Jenkins pipeline for SystemC_Accelerator_Model.
//
// Stages: configure and build (Release, -Wall -Wextra -Wpedantic) -> GoogleTest with
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
        string(name: 'SYSTEMC_HOME', defaultValue: "${env.HOME}/.local/opt/systemc", description: 'SystemC install')
        string(name: 'PERF_MARGIN', defaultValue: '0.5', description: 'Allowed slow-down against the baseline')
    }

    options {
        buildDiscarder(logRotator(numToKeepStr: '30'))
        timeout(time: 60, unit: 'MINUTES')
    }

    environment {
        LD_LIBRARY_PATH = "${params.SYSTEMC_HOME}/lib"
    }

    stages {
        stage('Build') {
            steps {
                sh '''
                    python3 -m venv .venv
                    .venv/bin/pip install -q cmake "fhe-sim @ git+https://github.com/BrendanJamesLynskey/FHE_Accelerator_Sim"
                    .venv/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$SYSTEMC_HOME
                    .venv/bin/cmake --build build -j2
                '''
            }
        }

        stage('Unit and agreement tests') {
            steps {
                sh './build/accel_tests --gtest_output=xml:gtest-release.xml'
            }
        }

        stage('Sanitizers') {
            steps {
                sh '''
                    .venv/bin/cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DACCEL_SANITIZE=ON \
                        -DCMAKE_PREFIX_PATH=$SYSTEMC_HOME
                    .venv/bin/cmake --build build-asan -j2
                    ./build-asan/accel_tests --gtest_output=xml:gtest-asan.xml
                '''
            }
        }

        stage('Agreement with the current SimPy model') {
            steps {
                sh '.venv/bin/python ci/agreement.py'
            }
        }

        stage('Performance gate') {
            steps {
                sh ".venv/bin/python ci/perf_gate.py --margin ${params.PERF_MARGIN}"
            }
            post {
                always { archiveArtifacts artifacts: 'perf_report.md', allowEmptyArchive: true }
            }
        }

        stage('Results') {
            steps {
                sh '.venv/bin/python examples/results.py > /dev/null'
                archiveArtifacts artifacts: 'examples/results.md'
            }
        }

        stage('Nightly quantum sweep') {
            when { expression { params.NIGHTLY } }
            steps {
                sh '.venv/bin/python ci/quantum_sweep.py'
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
