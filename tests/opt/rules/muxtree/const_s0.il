attribute \top 1
module \t
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire output 4 \y
  wire \s
  wire $n7
  cell $mux $c6
    parameter \WIDTH 1
    connect \A \c
    connect \B \b
    connect \S \a
    connect \Y $n7
  end
  connect \y $n7
  connect \s 1'0
end
