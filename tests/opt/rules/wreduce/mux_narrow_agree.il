attribute \top 1
module \mux_narrow_agree
  wire input 1 \s
  wire input 2 \c
  wire width 3 input 3 \a
  wire width 3 input 4 \b
  wire width 4 output 5 \y
  wire $n13
  wire $n14
  wire $n15
  cell $mux $c7
    parameter \WIDTH 3
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y { $n15 $n14 $n13 }
  end
  connect \y { \c $n15 $n14 $n13 }
end
