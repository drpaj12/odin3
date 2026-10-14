attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire input 4 \c
  wire output 5 \y
  cell $mux $c7
    parameter \WIDTH 1
    connect \A \c
    connect \B \a
    connect \S \s
    connect \Y \y
  end
end
