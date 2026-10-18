attribute \top 1
module \m
  wire input 1 \a
  wire input 2 \b
  wire input 3 \s
  wire output 4 \y
  cell $mux $c5
    parameter \WIDTH 1
    connect \A \a
    connect \B \b
    connect \S \s
    connect \Y \y
  end
end
