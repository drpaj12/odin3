attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire input 4 \c
  wire input 5 \d
  wire output 6 \y
  cell $mux $c9
    parameter \WIDTH 1
    connect \A \d
    connect \B \a
    connect \S \s
    connect \Y \y
  end
end
