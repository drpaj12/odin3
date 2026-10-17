attribute \top 1
module \mux_narrow_unused
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 2 output 4 \y
  wire width 4 \t
  wire $n17
  wire $n18
  cell $mux $c7
    parameter \WIDTH 2
    connect \A \b [1:0]
    connect \B \a [1:0]
    connect \S \s
    connect \Y { $n18 $n17 }
  end
  connect \y { $n18 $n17 }
  connect \t { 2'xx $n18 $n17 }
end
