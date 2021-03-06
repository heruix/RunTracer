# Author: Ben Nagy
# Copyright: Copyright (c) Ben Nagy, 2006-2010.
# License: The MIT License
# (See README.TXT or http://www.opensource.org/licenses/mit-license.php for details.)

require 'fileutils'
require 'rubygems'
require 'trollop'
require File.dirname( __FILE__ ) + '/stalk_trace_inserter'
require File.dirname( __FILE__ ) + '/stalk_trace_processor'
require File.dirname( __FILE__ ) + '/iterpi'

OPTS=Trollop::options do
    opt :port, "Port to connect to", :type=>:integer, :default=>11300
    opt :servers, "Beanstalk servers to connect to", :type=>:strings, :default=>["127.0.0.1"]
    opt :untraced, "Untraced file directory", :type=>:string, :required=>:true
    opt :modules, "Show offsets for these modules (other addresses raw)", :type=>:strings
    opt :keep, "Don't overwrite existing trace results", :type=>:boolean
    opt :pi, "Calculate Pi.", :type=>:boolean
    opt :output, "Output file basename foo becomes foo-traces.tch", :type=>:string, :required=>:true
    opt :debug, "Enable debug output", :type=>:boolean
end

Dir.mkdir OPTS[:untraced] unless File.directory? OPTS[:untraced]
Dir.mkdir OPTS[:output] unless File.directory? OPTS[:output]
Thread.abort_on_exception=true

# Objects shared between the two threads. No need for Mutex.
inserter=StalkTraceInserter.new( OPTS[:servers], OPTS[:port], OPTS[:debug] )
processor=StalkTraceProcessor.new( OPTS[:servers], OPTS[:port], OPTS[:output], OPTS[:debug] )
iterpi=IterPi.new if OPTS[:pi]
queue_size=0

trap("INT") { 
    processor.close_databases 
    puts "Pi is #{iterpi.pi}" if OPTS[:pi]
    exit 
}

# Insert files in this thread
Thread.new do
    begin
        Dir.glob( File.join(File.expand_path(OPTS[:untraced]), "*.doc"), File::FNM_DOTMATCH ).each {|fname|
            iterpi.update( File.basename(fname, ".doc") ) if OPTS[:pi]
            if OPTS[:keep]
                if processor.has_file? File.basename( fname )
                    print '.' if OPTS[:debug]
                    next
                end
            end
            sleep 1 until queue_size < 100
            inserter.insert( File.open(fname, "rb") {|ios| ios.read}, File.basename( fname ), (OPTS[:modules]||[]) )
            queue_size+=1
        }
        inserter.finish
    rescue
        raise $!
    end
end

# Process results in this thread. We join this thread, because it doesn't exit
# until the work is finished.
Thread.new do
    loop do
        processor.process_next
        queue_size-=1
        if inserter.finished? && queue_size==0
            puts "Sweet, we're done with #{inserter.inserted_count} files."
            Thread.exit
        end
    end
end.join

