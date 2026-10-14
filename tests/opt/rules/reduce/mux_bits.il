attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire width 2 output 4 \y
  wire $n6
  cell $mux $c6
    parameter \WIDTH 1
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y $n6
  end
  connect \y { $n6 $n6 }
end
