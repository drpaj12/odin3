attribute \top 1
module \m
  wire input 1 \en
  wire input 2 \d
  wire output 3 \q
  cell $dlatch $c16
    parameter \WIDTH 1
    parameter \EN_POLARITY 1
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
