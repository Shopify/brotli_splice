# frozen_string_literal: true

require "bundler/gem_tasks"
require "rake/testtask"

task :compile do
  Dir.chdir("ext/brotli_splice") do
    ruby "extconf.rb"
    sh "make"
  end
end

Rake::TestTask.new(:test => :compile) do |test|
  test.libs << "lib"
  test.pattern = "test/**/*_test.rb"
end

task default: :test
